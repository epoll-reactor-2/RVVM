/*
pci-bus.c - Peripheral Component Interconnect Bus
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "pci-bus.h"
#include "fdtlib.h"
#include "vector.h"
#include "spinlock.h"
#include "utils.h"
#include "bit_ops.h"
#include "mem_ops.h"

// PCI Configuration Space Registers
#define PCI_REG_DEV_VEN_ID    0x0  // Device, Vendor ID
#define PCI_REG_STATUS_CMD    0x4  // Status, Command
#define PCI_REG_CLASS_REV     0x8  // Class, Subclass, Prog IF, revision
#define PCI_REG_INFO          0xC  // BIST, Header type, Latency, Cache line size
#define PCI_REG_BAR0          0x10 // BAR0 Base Address
#define PCI_REG_BAR1          0x14 // BAR1 Base Address (Or upper 32 bits of BAR0)
#define PCI_REG_BAR2          0x18 // BAR2 Base Address (Or upper 32 bits of BAR1)
#define PCI_REG_BAR3          0x1C // BAR3 Base Address (Or upper 32 bits of BAR2)
#define PCI_REG_BAR4          0x20 // BAR4 Base Address (Or upper 32 bits of BAR3)
#define PCI_REG_BAR5          0x24 // BAR5 Base Address (Or upper 32 bits of BAR4)
#define PCI_REG_SSID_SVID     0x2C // Subsystem ID, Subsystem Vendor ID
#define PCI_REG_EXPANSION_ROM 0x30 // Expansion ROM Base Address
#define PCI_REG_CAP_PTR       0x34 // Capabilities List Pointer
#define PCI_REG_IRQ_PIN_LINE  0x3C // Interrupt PIN, Interrupt Line

// Command bits
#define PCI_CMD_IO_SPACE      0x1   // Accessible through IO ports
#define PCI_CMD_MEM_SPACE     0x2   // Accessible through MMIO
#define PCI_CMD_BUS_MASTER    0x4   // May use DMA
#define PCI_CMD_INTX_DISABLE  0x400 // INTx Interrupt Disabled

#define PCI_CMD_DEFAULT (PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER)
#define PCI_CMD_MASK    (PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE)

// Status bits
#define PCI_STATUS_INTX 0x8  // INTx Interrupt Status
#define PCI_STATUS_CAP  0x10 // Capabilities List Present

// Header type bits
#define PCI_HEADER_PCI_PCI   0x1  // PCI-PCI Bridge
#define PCI_HEADER_MULTIFUNC 0x80 // Multi-function device

// BAR bits
#define PCI_BAR_IO_SPACE 0x1
#define PCI_BAR_64_BIT   0x4
#define PCI_BAR_PREFETCH 0x8

// Expansion ROM bits
#define PCI_EXPANSION_ROM_ENABLED 0x1

#define PCI_CAP_LIST_OFF 0x80 // Capabilities List Offset
#define PCI_MSI_CAP_OFF  0xC0 // MSI Capability Offset

// PCI Express capability port type
#define PCIE_CAP_PORT_ENDPOINT       0x0
#define PCIE_CAP_ROOT_PORT           0x4
#define PCIE_CAP_UPSTREAM_SWITCH     0x5
#define PCIE_CAP_DOWNSTREAM_SWITCH   0x6
#define PCIE_CAP_INTEGRATED_ENDPOINT 0x9

static const uint32_t pci_express_caps_ro[] = {
    0x0102C010, // [80] PCI Express (v2) Endpoint, IntMsgNum 0
    0x00008002, // DevCap: MaxPayload 512 bytes, RBE+
    0x00002050, // DevCtl: RlxdOrd+
    0x01800D02, // LnkCap: Speed 5GT/s, Width x16
    0x01020003, // LnkSta: Speed 5GT/s, Width x16
    0x00020060,
    0x00200028,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000002,
    0x00000000,
    0x00000000,
    0x00000000,
    0x00000000,

    0x0003D001, // [C0] Power Management version 3
    0x00000008, // NoSoftRst+
    0x00000000,
    0x00000000,

    0x01800005, // [D0] MSI: Enable- Count=1/1 Maskable+ 64bit+
    0x00000000, // Message Address Low
    0x00000000, // Message Address High
    0x00000000, // Message Data
    0x00000000, // Mask
    0x00000000, // Pending

};

struct rvvm_pci_function {
    pci_bus_t* bus;
    rvvm_mmio_dev_t* bar[PCI_FUNC_BARS];
    rvvm_mmio_dev_t* expansion_rom;
    pci_bus_addr_t addr;

    // Atomic variables
    uint32_t command;
    uint32_t irq_line;
    uint32_t bridge_io;
    uint32_t bridge_mem;

    // RO attributes
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint8_t  prog_if;
    uint8_t  rev;
    uint8_t  irq_pin;
};

struct rvvm_pci_device {
    pci_bus_t* bus;
    pci_func_t* func[PCI_DEV_FUNCS];
    pci_bus_addr_t addr;
};

struct pci_bus {
    rvvm_machine_t* machine;
    plic_ctx_t* plic;

    vector_t(pci_dev_t*) dev;
    spinlock_t lock;

    uint32_t irq[PCI_BUS_IRQS];

    rvvm_addr_t io_addr;
    size_t      io_len;
    rvvm_addr_t mem_addr;
    size_t      mem_len;
};

// Free a PCI function description that we've failed to attach
static void pci_free_func_desc(const pci_func_desc_t* desc)
{
    if (desc) {
        for (size_t bar_id = 0; bar_id < PCI_FUNC_BARS; ++bar_id) {
            rvvm_cleanup_mmio_desc(&desc->bar[bar_id]);
        }
    }
}

// Free a PCI device description that we've failed to attach
static void pci_free_dev_desc(const pci_dev_desc_t* desc)
{
    if (desc) {
        for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
            pci_free_func_desc(desc->func[func_id]);
        }
    }
}

// Linearize PCI bus addesses into internal vector
static inline size_t pci_bus_addr_to_id_internal(pci_bus_addr_t bus_addr)
{
    return (bus_addr < 0x100) ? (bus_addr >> 3) : ((bus_addr >> 8) + PCI_BUS_DEVS);
}

static inline size_t pci_bus_addr_valid(pci_bus_addr_t bus_addr)
{
    return (bus_addr < 0x100) || !(bus_addr & 0xF8);
}

// Assign a PCI device handle to a PCI slot (Must be called under locking)
static inline bool pci_set_bus_dev_internal(pci_bus_t* bus, pci_bus_addr_t bus_addr, pci_dev_t* dev)
{
    size_t dev_id = pci_bus_addr_to_id_internal(bus_addr);
    if (bus && (dev_id >= vector_size(bus->dev) || (!!vector_at(bus->dev, dev_id) != !!dev))) {
        vector_put(bus->dev, dev_id, dev);
        return true;
    }
    return false;
}

// Free a PCI function (Must be called under locking), omit removing mmio on machine cleanup
static void pci_free_func_internal(pci_func_t* func, bool remove_mmio)
{
    if (func) {
        if (remove_mmio) {
            // Omit double-removing MMIO on final machine cleanup stage
            for (size_t bar_id = 0; bar_id < PCI_FUNC_BARS; ++bar_id) {
                rvvm_remove_mmio(func->bar[bar_id]);
            }
        }
        free(func);
    }
}

// Free a PCI device (Must be called under locking), omit removing mmio on machine cleanup
static void pci_free_dev_internal(pci_dev_t* dev, bool remove_mmio)
{
    if (dev) {
        for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
            pci_free_func_internal(dev->func[func_id], remove_mmio);
        }
        free(dev);
    }
}

// Get INTx IRQ pin routing id for a function
static inline size_t pci_func_irq_pin_id(pci_func_t* func)
{
    return ((func->addr >> 3) + func->irq_pin + 3) & 3;
}

// Assign MMIO address for a BAR
static rvvm_addr_t pci_assign_mmio_addr(pci_bus_t* bus, size_t bar_size)
{
    size_t align_size = bit_next_pow2(EVAL_MAX(bar_size, 0x1000));
    rvvm_addr_t addr = bus->mem_addr;
    while (true) {
        rvvm_addr_t tmp = rvvm_mmio_zone_auto(bus->machine, addr, align_size);
        if (tmp == addr) {
            break;
        }
        addr = tmp + ((align_size - tmp) & (align_size - 1));
    }
    return addr;
}

// Attach a preconfigured BAR to the PCI host
static rvvm_mmio_dev_t* pci_attach_bar(pci_bus_t* bus, rvvm_mmio_dev_t bar)
{
    bar.addr = pci_assign_mmio_addr(bus, bar.size);
    return rvvm_attach_mmio(bus->machine, &bar);
}

static pci_func_t* pci_attach_func_internal(pci_bus_t* bus, const pci_func_desc_t* desc, pci_bus_addr_t bus_addr)
{
    pci_func_t* func = safe_new_obj(pci_func_t);
    func->bus = bus;

    func->vendor_id  = desc->vendor_id;
    func->device_id  = desc->device_id;
    func->class_code = desc->class_code;
    func->prog_if    = desc->prog_if;
    func->rev        = desc->rev;
    func->irq_pin    = desc->irq_pin;

    func->addr  = bus_addr;

    func->command  = PCI_CMD_DEFAULT;
    func->irq_line = bus->irq[pci_func_irq_pin_id(func)];

    for (size_t bar_id = 0; bar_id < PCI_FUNC_BARS; ++bar_id) {
        if (desc->bar[bar_id].size) {
            func->bar[bar_id] = pci_attach_bar(bus, desc->bar[bar_id]);
            if (func->bar[bar_id] == NULL) {
                // Failed to attach function BAR
                free(func);
                return NULL;
            }
        }
    }

    if (desc->expansion_rom.size) {
        func->expansion_rom = pci_attach_bar(bus, desc->expansion_rom);
        if (func->expansion_rom == NULL) {
            // Failed to attach function expansion ROM
            free(func);
            return NULL;
        }
    }

    return func;
}

// Check whether this BAR is an upper half for a previous 64-bit BAR
static inline size_t pci_bar_is_upper_half(pci_func_t* func, size_t bar_id)
{
    return bar_id && !func->bar[bar_id] && func->bar[bar_id - 1];
}

// Check whether this BAR is eligible to be 64-bit wide
static inline bool pci_bar_is_64bit(pci_func_t* func, size_t bar_id)
{
    return bar_id + 1 < PCI_FUNC_BARS && func->bar[bar_id] && !func->bar[bar_id + 1];
}

// Get effective BAR (Returns same device bar for low/high parts)
static inline rvvm_mmio_dev_t* pci_effective_bar(pci_func_t* func, size_t bar_id)
{
    if (pci_bar_is_upper_half(func, bar_id)) {
        return func->bar[bar_id - 1];
    }
    return func->bar[bar_id];
}

static bool pci_bus_read(rvvm_mmio_dev_t* mmio_dev, void* data, size_t offset, uint8_t size)
{
    pci_bus_t* bus = mmio_dev->data;
    pci_bus_addr_t bus_addr = offset >> 12;
    size_t reg = offset & 0xFFC;
    uint32_t val = 0;
    UNUSED(size);

    pci_func_t* func = pci_get_bus_func(bus, bus_addr);
    if (!func) {
        // Nonexistent devices have all 0xFFFF in their conf space
        write_uint32_le(data, 0xFFFFFFFF);
        return true;
    }

    switch (reg) {
        case PCI_REG_DEV_VEN_ID:
            val = (func->vendor_id | (uint32_t)func->device_id << 16);
            break;
        case PCI_REG_STATUS_CMD: {
            val = atomic_load_uint32_relax(&func->command);
            if (!(val & PCI_CMD_INTX_DISABLE)) {
                // Always report INTx+ whenever DisINTx- for INTx emulation
                val |= PCI_STATUS_INTX << 16;
            }
            if (bus_addr) {
                // Host bridge doesn't have capabilities
                val |= PCI_STATUS_CAP << 16;
            }
            break;
        }
        case PCI_REG_CLASS_REV:
            val = func->rev | (((uint32_t)func->prog_if) << 8) | (((uint32_t)func->class_code) << 16);
            break;
        case PCI_REG_INFO: {
            // Advertise 64-byte cache lines
            val = 16;
            pci_dev_t* dev = pci_get_bus_device(bus, bus_addr);
            for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
                if (pci_get_device_func(dev, func_id) && (func_id != (bus_addr & 0x7))) {
                    // This is a multi-function device
                    val |= (PCI_HEADER_MULTIFUNC << 16);
                    break;
                }
            }
            if (func->class_code == 0x0604) {
                // This is a PCI-PCI bridge (PCI Express Root Port)
                val |= (PCI_HEADER_PCI_PCI << 16);
            }
            break;
        }
        case PCI_REG_IRQ_PIN_LINE:
            val = (atomic_load_uint32_relax(&func->irq_line) | ((uint32_t)func->irq_pin) << 8);
            break;
        case PCI_REG_BAR0:
        case PCI_REG_BAR1:
        case PCI_REG_BAR2:
        case PCI_REG_BAR3:
        case PCI_REG_BAR4:
        case PCI_REG_BAR5: {
            uint8_t bar_id = (reg - PCI_REG_BAR0) >> 2;
            // Only BAR0 & BAR1 exist for a PCI-PCI bridge
            if (func->class_code != 0x0604 || bar_id < 2) {
                rvvm_mmio_dev_t* bar = pci_effective_bar(func, bar_id);
                if (bar) {
                    if (pci_bar_is_upper_half(func, bar_id)) {
                        // This is an upper half of a 64-bit BAR
                        val = bar->addr >> 32;
                    } else {
                        val = bar->addr;
                        if (pci_bar_is_64bit(func, bar_id)) {
                            // This BAR supports 64-bit addressing
                            val |= PCI_BAR_64_BIT;
                        }
                    }
                }
            } else {
                // PCIe Root Ports within 00:XX.x cover every other bus in the system
                uint8_t secondary_bus = (bus_addr >> 3) + (bus_addr & 0x7);
                switch (bar_id) {
                    case 0x2:
                        val = (secondary_bus << 16) | (secondary_bus << 8);
                        break;
                    case 0x3:
                        val = atomic_load_uint32_relax(&func->bridge_io);
                        break;
                    case 0x4:
                        val = atomic_load_uint32_relax(&func->bridge_mem);
                        break;
                }
            }
            break;
        }
        case PCI_REG_SSID_SVID:
            val = 0x510010dc;
            break;
        case PCI_REG_EXPANSION_ROM:
            if (func->expansion_rom) {
                val = func->expansion_rom->addr | PCI_EXPANSION_ROM_ENABLED;
            }
            break;
        case PCI_REG_CAP_PTR:
            if (bus_addr) {
                val = PCI_CAP_LIST_OFF;
            }
            break;
        default:
            if (bus_addr) {
                // Handle PCI capabilities
                size_t cap_id = (reg - PCI_CAP_LIST_OFF) >> 2;
                if (cap_id < STATIC_ARRAY_SIZE(pci_express_caps_ro)) {
                    val = pci_express_caps_ro[cap_id];
                }
                if (!cap_id) {
                    if (func->class_code == 0x0604) {
                        // This is a PCI-PCI bridge (PCI Express Root Port)
                        val |= (PCIE_CAP_ROOT_PORT << 20);
                    } else if (!(bus_addr >> 8)) {
                        // This is an integrated endpoint on bus 00
                        val |= (PCIE_CAP_INTEGRATED_ENDPOINT << 20);
                    }
                }
            }
            break;
    }

    write_uint32_le(data, val);

    return true;
}

static bool pci_bus_write(rvvm_mmio_dev_t* mmio_dev, void* data, size_t offset, uint8_t size)
{
    pci_bus_t* bus = mmio_dev->data;
    pci_bus_addr_t bus_addr = offset >> 12;
    size_t reg = offset & 0xFFC;
    uint32_t val = read_uint32_le(data);
    UNUSED(size);

    pci_func_t* func = pci_get_bus_func(bus, bus_addr);
    if (!func) {
        // No such device
        return true;
    }

    switch (reg) {
        case PCI_REG_STATUS_CMD:
            atomic_store_uint32_relax(&func->command, val & PCI_CMD_MASK);
            break;
        case PCI_REG_BAR0:
        case PCI_REG_BAR1:
        case PCI_REG_BAR2:
        case PCI_REG_BAR3:
        case PCI_REG_BAR4:
        case PCI_REG_BAR5: {
            uint8_t bar_id = (reg - PCI_REG_BAR0) >> 2;
            // Only BAR0 & BAR1 exist for a PCI-PCI bridge
            if (func->class_code != 0x0604 || bar_id < 2) {
                rvvm_mmio_dev_t* bar = pci_effective_bar(func, bar_id);
                if (bar) {
                    rvvm_addr_t bar_addr = bar->addr;
                    size_t bar_size = bit_next_pow2(bar->size);
                    if (pci_bar_is_upper_half(func, bar_id)) {
                        // This is an upper half of a 64-bit BAR
                        bar_addr = bit_replace(bar_addr, 32, 32, val);
                    } else {
                        // Mask lower option bits and align to page
                        bar_addr = bit_replace(bar_addr, 0, 32, val & ~0xFFFU);
                    }

                    // Align address to BAR size (Must be power of 2)
                    bar->addr = bar_addr & ~(bar_size - 1);

                    atomic_fence();
                }
            } else {
                switch (bar_id) {
                    case 0x3:
                        atomic_store_uint32_relax(&func->bridge_io, val);
                        break;
                    case 0x4:
                        atomic_store_uint32_relax(&func->bridge_mem, val);
                        break;
                }
            }
            break;
        }
        case PCI_REG_EXPANSION_ROM:
            if (func->expansion_rom) {
                rvvm_addr_t rom_addr = val & ~0xFFFU;
                size_t rom_size = bit_next_pow2(func->expansion_rom->size);
                func->expansion_rom->addr = rom_addr & ~(rom_size - 1);
                atomic_fence();
            }
            break;
        case PCI_REG_IRQ_PIN_LINE:
            atomic_store_uint32_relax(&func->irq_line, (uint8_t)val);
            break;
    }

    return true;
}

static void pci_bus_remove(rvvm_mmio_dev_t* mmio_dev)
{
    pci_bus_t* bus = mmio_dev->data;
    vector_foreach(bus->dev, i) {
        pci_free_dev_internal(vector_at(bus->dev, i), false);
    }
    vector_free(bus->dev);
    free(bus);
}

static const rvvm_mmio_type_t pci_bus_type = {
    .name = "pci_bus",
    .remove = pci_bus_remove,
};

PUBLIC pci_bus_t* pci_bus_init(rvvm_machine_t* machine, plic_ctx_t* plic, const uint32_t irqs[4],
                               rvvm_addr_t base_addr, size_t bus_count,
                               rvvm_addr_t io_addr, size_t io_len,
                               rvvm_addr_t mem_addr, size_t mem_len)
{
    pci_bus_t* bus = safe_new_obj(pci_bus_t);
    bus->machine = machine;
    bus->plic = plic;

    for (size_t i = 0; i < PCI_BUS_IRQS; ++i) {
        bus->irq[i] = irqs[i];
    }

    bus->io_addr = io_addr;
    bus->io_len = io_len;
    bus->mem_addr = mem_addr;
    bus->mem_len = mem_len;

    rvvm_mmio_dev_t pci_bus_mmio = {
        .addr = base_addr,
        .size = (bus_count << 20),
        .data = bus,
        .type = &pci_bus_type,
        .read = pci_bus_read,
        .write = pci_bus_write,
        .min_op_size = 4,
        .max_op_size = 4,
    };

    if (!rvvm_attach_mmio(machine, &pci_bus_mmio)) {
        // Failed to attach the PCI bus
        return NULL;
    }

    // Host Bridge: SiFive, Inc. FU740-C000 RISC-V SoC PCI Express x8
    pci_func_desc_t bridge_desc = { .vendor_id = 0xF15E, .class_code = 0x0600, };
    pci_attach_func(bus, &bridge_desc);

    if (rvvm_has_arg("pcie_ports")) {
        // Root Ports
        pci_func_desc_t root_port_desc = { .vendor_id = 0x1556, .device_id = 0xbe00, .class_code = 0x0604, .irq_pin = PCI_IRQ_PIN_INTA, };
        for (pci_bus_addr_t bus_addr = 1; bus_addr < PCI_DEV_FUNCS; ++bus_addr) {
            pci_attach_func_at(bus, &root_port_desc, bus_addr);
        }
    }

    rvvm_set_pci_bus(machine, bus);

#ifdef USE_FDT
    struct fdt_node* pci_node = fdt_node_create_reg("pci", base_addr);
    fdt_node_add_prop_u32(pci_node, "#address-cells", 3);
    fdt_node_add_prop_u32(pci_node, "#size-cells", 2);
    fdt_node_add_prop_u32(pci_node, "#interrupt-cells", 1);
    fdt_node_add_prop_str(pci_node, "device_type", "pci");
    fdt_node_add_prop_reg(pci_node, "reg", base_addr, pci_bus_mmio.size);
    fdt_node_add_prop_str(pci_node, "compatible", "pci-host-ecam-generic");
    fdt_node_add_prop(pci_node, "dma-coherent", NULL, 0);

    #define FDT_ADDR(addr) (((uint64_t)(addr)) >> 32), ((uint32_t)(addr))

    uint32_t bus_range[2] = { 0, bus_count - 1 };
    fdt_node_add_prop_cells(pci_node, "bus-range", bus_range, 2);

    // Range header: ((cacheable) << 30 | (space) << 24 | (bus) << 16 | (dev) << 11 | (fun) << 8 | (reg))
    uint32_t ranges[14] = {
        0x1000000, FDT_ADDR(0),        FDT_ADDR(io_addr),  FDT_ADDR(io_len),
        0x2000000, FDT_ADDR(mem_addr), FDT_ADDR(mem_addr), FDT_ADDR(mem_len),
    };
    fdt_node_add_prop_cells(pci_node, "ranges", ranges + (io_len ? 0 : 7), io_len ? 14 : 7);

    // Crossing-style IRQ routing for IRQ balancing
    // INTA of dev 2 routes the same way as INTB of dev 1, etc
    uint32_t plic_handle = plic_get_phandle(plic);
    uint32_t interrupt_map[96] = {
        0x0000, 0, 0, 1, plic_handle, bus->irq[0],
        0x0000, 0, 0, 2, plic_handle, bus->irq[1],
        0x0000, 0, 0, 3, plic_handle, bus->irq[2],
        0x0000, 0, 0, 4, plic_handle, bus->irq[3],
        0x0800, 0, 0, 1, plic_handle, bus->irq[1],
        0x0800, 0, 0, 2, plic_handle, bus->irq[2],
        0x0800, 0, 0, 3, plic_handle, bus->irq[3],
        0x0800, 0, 0, 4, plic_handle, bus->irq[0],
        0x1000, 0, 0, 1, plic_handle, bus->irq[2],
        0x1000, 0, 0, 2, plic_handle, bus->irq[3],
        0x1000, 0, 0, 3, plic_handle, bus->irq[0],
        0x1000, 0, 0, 4, plic_handle, bus->irq[1],
        0x1800, 0, 0, 1, plic_handle, bus->irq[3],
        0x1800, 0, 0, 2, plic_handle, bus->irq[0],
        0x1800, 0, 0, 3, plic_handle, bus->irq[2],
        0x1800, 0, 0, 4, plic_handle, bus->irq[1],
    };
    fdt_node_add_prop_cells(pci_node, "interrupt-map", interrupt_map, 96);

    uint32_t interrupt_mask[4] = { 0x1800, 0, 0, 7 };
    fdt_node_add_prop_cells(pci_node, "interrupt-map-mask", interrupt_mask, 4);

    fdt_node_add_child(rvvm_get_fdt_soc(machine), pci_node);
#endif
    return bus;
}

PUBLIC pci_bus_t* pci_bus_init_auto(rvvm_machine_t* machine)
{
    plic_ctx_t* plic = rvvm_get_plic(machine);
    uint32_t irqs[PCI_BUS_IRQS] = {0};
    size_t bus_count = 256; // TODO: Support more than 1 working bus
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, PCI_BASE_DEFAULT_MMIO, bus_count << 20);
    for (size_t i = 0; i < PCI_BUS_IRQS; ++i) {
        irqs[i] = plic_alloc_irq(plic);
    }
    return pci_bus_init(machine, plic, irqs, addr, bus_count,
                        PCI_IO_DEFAULT_ADDR, PCI_IO_DEFAULT_SIZE,
                        PCI_MEM_DEFAULT_MMIO, PCI_MEM_DEFAULT_SIZE);
}

PUBLIC pci_dev_t* pci_attach_func(pci_bus_t* bus, const pci_func_desc_t* desc)
{
    pci_dev_desc_t dev_desc = {
        .func[0] = desc,
    };
    return pci_attach_multifunc(bus, &dev_desc);
}

PUBLIC pci_dev_t* pci_attach_multifunc(pci_bus_t* bus, const pci_dev_desc_t* desc)
{
    for (pci_bus_addr_t bus_addr = 0x0; bus_addr < 0x100; bus_addr += PCI_DEV_FUNCS) {
        if (!pci_get_bus_device(bus, bus_addr)) {
            // Found a free integrated PCI slot
            return pci_attach_multifunc_at(bus, desc, bus_addr);
        }
    }
    if (rvvm_has_arg("pcie_ports")) {
        for (pci_bus_addr_t bus_addr = 0x100; bus_addr < 0x800; bus_addr += 0x100) {
            if (!pci_get_bus_device(bus, bus_addr)) {
                // Found a free PCI port
                return pci_attach_multifunc_at(bus, desc, bus_addr);
            }
        }
    }
    rvvm_error("Too many PCI devices attached!");
    pci_free_dev_desc(desc);
    return NULL;
}

PUBLIC pci_dev_t* pci_attach_func_at(pci_bus_t* bus, const pci_func_desc_t* desc, pci_bus_addr_t bus_addr)
{
    pci_dev_t* dev = pci_get_bus_device(bus, bus_addr);
    uint8_t func_id = bus_addr & 0x7;
    if (dev) {
        // Inject a function into an existing device
        pci_func_t* func = pci_attach_func_internal(bus, desc, bus_addr);
        if (!func) {
            // Failed to attach function
            pci_free_func_desc(desc);
            return NULL;
        }

        // Pause the vCPUs and attach the device
        spin_lock(&bus->lock);
        bool was_running = rvvm_pause_machine(bus->machine);
        if (dev->func[func_id]) {
            // Another function already in the slot
            rvvm_error("PCI function %02x:%02x.%01x is busy!", (bus_addr >> 8) & 0xFF, (bus_addr >> 3) & 0x1F, func_id);
            pci_free_func_internal(func, true);
            dev = NULL;
        } else {
            dev->func[func_id] = func;
        }
        if (was_running) rvvm_start_machine(bus->machine);
        spin_unlock(&bus->lock);
        return dev;
    } else {
        // Create a new device
        pci_dev_desc_t dev_desc = {0};
        dev_desc.func[func_id] = desc;
        return pci_attach_multifunc_at(bus, &dev_desc, bus_addr & ~0x7U);
    }
}

PUBLIC pci_dev_t* pci_attach_multifunc_at(pci_bus_t* bus, const pci_dev_desc_t* desc, pci_bus_addr_t bus_addr)
{
    if (bus == NULL) {
        pci_free_dev_desc(desc);
        return NULL;
    }

    pci_dev_t* dev = safe_new_obj(pci_dev_t);
    dev->bus = bus;
    dev->addr = bus_addr;

    for (size_t func_id = 0; func_id < PCI_DEV_FUNCS; ++func_id) {
        if (desc->func[func_id]) {
            pci_func_t* func = pci_attach_func_internal(bus, desc->func[func_id], bus_addr + func_id);
            dev->func[func_id] = func;
            if (!func) {
                // Failed to attach function
                pci_free_dev_internal(dev, true);
                return NULL;
            }
        }
    }

    // Pause the vCPUs and attach the device
    spin_lock(&bus->lock);
    bool was_running = rvvm_pause_machine(bus->machine);
    if (!pci_set_bus_dev_internal(bus, bus_addr, dev)) {
        // Another device already in the slot
        rvvm_error("PCI slot %02x:%02x.0 is busy!", (bus_addr >> 8) & 0xFF, (bus_addr >> 3) & 0x1F);
        pci_free_dev_internal(dev, true);
        dev = NULL;
    }
    if (was_running) rvvm_start_machine(bus->machine);
    spin_unlock(&bus->lock);

    return dev;
}

PUBLIC pci_dev_t* pci_get_bus_device(pci_bus_t* bus, pci_bus_addr_t bus_addr)
{
    size_t dev_id = pci_bus_addr_to_id_internal(bus_addr);
    pci_dev_t* dev = NULL;
    if (bus && pci_bus_addr_valid(bus_addr)) {
        spin_lock(&bus->lock);
        if (dev_id < vector_size(bus->dev)) {
            dev = vector_at(bus->dev, dev_id);
        }
        spin_unlock(&bus->lock);
    }
    return dev;
}

PUBLIC pci_func_t* pci_get_bus_func(pci_bus_t* bus, pci_bus_addr_t bus_addr)
{
    if (bus) {
        pci_dev_t* dev = pci_get_bus_device(bus, bus_addr);
        if (dev) {
            return pci_get_device_func(dev, bus_addr & 0x7);
        }
    }
    return NULL;
}

PUBLIC pci_bus_addr_t pci_get_device_bus_addr(pci_dev_t* dev)
{
    return dev ? dev->addr : 0;
}

PUBLIC pci_bus_addr_t pci_get_func_bus_addr(pci_func_t* func)
{
    return func ? func->addr : 0;
}

PUBLIC void pci_remove_device(pci_dev_t* dev)
{
    if (dev) {
        // Pause the vCPUs and remove the device
        pci_bus_t* bus = dev->bus;
        spin_lock(&bus->lock);
        bool was_running = rvvm_pause_machine(bus->machine);
        pci_set_bus_dev_internal(bus, pci_get_device_bus_addr(dev), NULL);
        pci_free_dev_internal(dev, true);
        if (was_running) rvvm_start_machine(bus->machine);
        spin_unlock(&bus->lock);
    }
}

PUBLIC pci_func_t* pci_get_device_func(pci_dev_t* dev, size_t func_id)
{
    if (dev) {
        return dev->func[func_id];
    }
    return NULL;
}

PUBLIC void pci_send_irq(pci_func_t* func, uint32_t msi_id)
{
    UNUSED(msi_id);
    if (likely(func)) {
        pci_bus_t* bus = func->bus;
        // Check IRQs enabled
        if (likely(func->irq_pin && !(atomic_load_uint32_relax(&func->command) & PCI_CMD_INTX_DISABLE))) {
            // Send IRQ
            uint32_t irq = bus->irq[pci_func_irq_pin_id(func)];
            plic_send_irq(bus->plic, irq);
        }
    }
}

PUBLIC void* pci_get_dma_ptr(pci_func_t* func, rvvm_addr_t addr, size_t size)
{
    if (likely(func && (atomic_load_uint32_relax(&func->command) & PCI_CMD_BUS_MASTER))) {
        // DMA requires bus mastering to be enabled
        return rvvm_get_dma_ptr(func->bus->machine, addr, size);
    }
    return NULL;
}
