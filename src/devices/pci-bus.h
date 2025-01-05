/*
pci-bus.h - Peripheral Component Interconnect Bus
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_PCI_BUS_H
#define RVVM_PCI_BUS_H

#include "rvvmlib.h"
#include "plic.h"

#define PCI_IRQ_PIN_INTA 1
#define PCI_IRQ_PIN_INTB 2
#define PCI_IRQ_PIN_INTC 3
#define PCI_IRQ_PIN_INTD 4

#define PCI_BUS_IRQS     4
#define PCI_BUS_DEVS     32
#define PCI_DEV_FUNCS    8
#define PCI_FUNC_BARS    6

// Pass in dev_desc->func[x].bar[y].addr to use 64-bit BAR
#define PCI_BAR_ADDR_64  0x64646464

// Default PCI bus settings
#define PCI_BASE_DEFAULT_MMIO 0x30000000
#define PCI_IO_DEFAULT_ADDR   0x03000000
#define PCI_IO_DEFAULT_SIZE   0x00010000
#define PCI_MEM_DEFAULT_MMIO  0x40000000
#define PCI_MEM_DEFAULT_SIZE  0x40000000

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint8_t  prog_if;
    uint8_t  rev;
    uint8_t  irq_pin;
    rvvm_mmio_dev_t bar[PCI_FUNC_BARS];
} pci_func_desc_t;

typedef struct {
    pci_func_desc_t func[PCI_DEV_FUNCS];
} pci_dev_desc_t;

typedef struct pci_device pci_dev_t;

// Passing irq = 0 implies auto-allocation of 4 IRQ lanes
PUBLIC pci_bus_t* pci_bus_init(rvvm_machine_t *machine, plic_ctx_t* plic,
                               uint32_t irq, bool ecam,
                               rvvm_addr_t base_addr, size_t bus_count,
                               rvvm_addr_t io_addr, size_t io_len,
                               rvvm_addr_t mem_addr, size_t mem_len);

PUBLIC pci_bus_t* pci_bus_init_auto(rvvm_machine_t* machine);

// Connect PCI device to the bus, use returned handle to send interrupts
PUBLIC pci_dev_t* pci_bus_add_device(pci_bus_t* bus, const pci_dev_desc_t* desc);

// Drives IRQ pin of the corresponding device function
PUBLIC void       pci_send_irq(pci_dev_t* dev, uint32_t func_id);
PUBLIC void       pci_clear_irq(pci_dev_t* dev, uint32_t func_id);

// Directly access physical memory of the device bus host (returns non-NULL on success)
PUBLIC void*      pci_get_dma_ptr(pci_dev_t* dev, rvvm_addr_t addr, size_t size);

PUBLIC void       pci_remove_device(pci_dev_t* dev);

#endif
