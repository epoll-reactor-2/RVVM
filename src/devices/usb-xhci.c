/*
usb-xhci.h - USB Extensible Host Controller Interface
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "usb-xhci.h"
#include "utils.h"
#include "mem_ops.h"

/*
 * Register regions
 */

#define XHCI_CAPABILITY_BASE  0x0    // Capability registers base offset
#define XHCI_OPERATIONAL_BASE 0x80   // Operational registers base offset
#define XHCI_PORT_REGS_BASE   0x400  // Port registers base offset
#define XHCI_RUNTIME_BASE     0x2000 // Runtime registers base offset
#define XHCI_DOORBELL_BASE    0x3000 // Doorbell registers base offset
#define XHCI_EXT_CAPS_BASE    0x8000 // Extended capabilities base offset

#define XHCI_PORT_REGS_SIZE 0x1000  // Port registers region size
#define XHCI_RUNTIME_SIZE   0x1000  // Runtime registers region size
#define XHCI_DOORBELL_SIZE  0x1000  // Doorbell registers region size
#define XHCI_BAR_SIZE       0x10000 // Size of XHCI MMIO BAR

/*
 * Capability registers
 */

#define XHCI_REG_CAPLENGTH_HCIVERSION 0x00 // Capability Register Length (byte 0), Interface Version Number (byte 2)
#define XHCI_REG_HCSPARAMS1           0x04 // Structural Parameters 1
#define XHCI_REG_HCSPARAMS2           0x08 // Structural Parameters 2
#define XHCI_REG_HCSPARAMS3           0x0C // Structural Parameters 3
#define XHCI_REG_HCCPARAMS1           0x10 // Capability Parameters
#define XHCI_REG_DBOFF                0x14 // Doorbell Offset
#define XHCI_REG_RTSOFF               0x18 // Runtime Registers Space Offset
#define XHCI_REG_HCCPARMS2            0x1C // Capability Parameters 2

/*
 * Capability constants
 */

#define XHCI_VERSION 0x0100 // XHCI v1.0.0

#define XHCI_MAX_SLOTS 0x20 // 32 slots max
#define XHCI_MAX_PORTS 0x20 // 32 ports max
#define XHCI_MAX_IRQS  0x01 // 1 interrupter

#define XHCI_CAPLEN_HCIVERSION (XHCI_OPERATIONAL_BASE | (XHCI_VERSION << 16))
#define XHCI_HCSPARAMS1        (XHCI_MAX_SLOTS | (XHCI_MAX_IRQS << 8) | (XHCI_MAX_PORTS << 24))

#define XHCI_HCCPARAMS1 ((XHCI_EXT_CAPS_BASE << 14) | 0x5) // 64-bit addressing, 64-byte context size

/*
 * Operational registers
 */

#define XHCI_REG_USBCMD   0x80 // USB Command
#define XHCI_REG_USBSTS   0x84 // USB Status
#define XHCI_REG_PAGESIZE 0x88 // Page Size
#define XHCI_REG_DNCTRL   0x94 // Device Notification Control
#define XHCI_REG_CRCR     0x98 // Command Ring Control
#define XHCI_REG_CRCR_H   0x9C
#define XHCI_REG_DCBAAP   0xB0 // Device Context Base Address Array Pointer
#define XHCI_REG_CONFIG   0xB8 // Configure

// USB Command values
#define XHCI_USBCMD_RS    0x1 // Run/Stop
#define XHCI_USBCMD_HCRST 0x2 // Host Controller Reset
#define XHCI_USBCMD_INTE  0x4 // Interrupter Enable
#define XHCI_USBCMD_HSEE  0x8 // Host System Error Enable

// USB Status values
#define XHCI_USBSTS_HCH  0x1   // HCHalted
#define XHCI_USBSTS_HSE  0x4   // Host System Error
#define XHCI_USBSTS_EINT 0x8   // Event interrupt
#define XHCI_USBSTS_PCD  0x10  // Port Change Detected
#define XHCI_USBSTS_CNR  0x800 // Controller Not Ready

// Command Ring Control values
#define XHCI_CRCR_RCS 0x1 // Ring Cycle State
#define XHCI_CRCR_CS  0x2 // Command Stop
#define XHCI_CRCR_CA  0x4 // Command Abort
#define XHCI_CRCR_CRR 0x8 // Command Ring Running

/*
 * Port registers
 */

// Each port occupies 0x10 bytes at XHCI_PORT_REGS_BASE
#define XHCI_REG_PORTSC    0x0 // Port Status and Control
#define XHCI_REG_PORTPMSC  0x4 // Port Power Management Status and Control
#define XHCI_REG_PORTLI    0x8 // Port Link Info
#define XHCI_REG_PORTHLPMC 0xC // Port Hardware LPM Control

#define XHCI_PORTSC_CCS 0x1      // Current Connect Status
#define XHCI_PORTSC_PED 0x2      // Port Enabled/Disabled
#define XHCI_PORTSC_PR  0x10     // Port Reset
#define XHCI_PORTSC_PP  0x200    // Port Power
#define XHCI_PORTSC_CSC 0x2000   // Connect Status Change
#define XHCI_PORTSC_PEC 0x4000   // Port Enabled/Disabled Change
#define XHCI_PORTSC_WRC 0x8000   // Warm Port Reset Change
#define XHCI_PORTSC_PRC 0x20000  // Port Reset Change
#define XHCI_PORTSC_PLC 0x40000  // Port Link State Change
#define XHCI_PORTSC_CEC 0x80000  // Port Config Error Change
#define XHCI_PORTSC_CAS 0x100000 // Cold Attach Status

/*
 * Runtime registers
 */

// Each interruptor occupies 0x20 bytes at XHCI_RUNTIME_BASE + 0x20
#define XHCI_REG_IMAN   0x0  // Interrupter Management
#define XHCI_REG_IMOD   0x4  // Interrupter Moderation
#define XHCI_REG_ERSTSZ 0x8  // Event Ring Segment Table Size
#define XHCI_REG_ERSTBA 0x10 // Event Ring Segment Table Base Address
#define XHCI_REG_ERDP   0x18 // Event Ring Dequeue Pointer

static const uint32_t xhci_ext_caps[] = {
    0x02000802, // Supported Protocol Capability: USB 2.0
    0x20425355, // "USB "
    0x30001001, // Compatible ports 0x01 - 0x10, 3 protocol speeds
    0x00000000,
    0x000C0021, // Protocol speed 1: 12  Mbps
    0x05DC0012, // Protocol speed 2: 1.5 Mbps
    0x01E00023, // Protocol speed 3: 480 Mbps
    0x00000000,

    0x03000002, // Supported Protocol Capability: USB 3.0
    0x20425355, // "USB "
    0x10000A11, // Compatible ports 0x11 - 0x1A, 1 speed mode
    0x00000000,
    0x00050134, // Protocol speed 4: 5 Gbps
    0x00000000,
};

typedef struct {
    pci_dev_t* pci_dev;

    uint32_t usbcmd;
    uint32_t dnctrl;
} xhci_bus_t;

static uint32_t xhci_port_reg_read(xhci_bus_t* xhci, size_t port_id, size_t port_off)
{
    UNUSED(xhci);
    UNUSED(port_id);
    if (port_id < 1)
    switch (port_off) {
        case XHCI_REG_PORTSC:
            return XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_PP;
    }
    return 0;
}

static bool xhci_pci_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    xhci_bus_t* xhci = dev->data;
    uint32_t val = 0;
    UNUSED(size);


    if ((offset - XHCI_DOORBELL_BASE) < XHCI_DOORBELL_SIZE) {
        //rvvm_warn("XHCI doorbell read %04x", (uint32_t)offset);
    } else if ((offset - XHCI_RUNTIME_BASE) < XHCI_RUNTIME_SIZE) {
        //rvvm_warn("XHCI runtime read %04x", (uint32_t)offset);
    } else if ((offset - XHCI_PORT_REGS_BASE) < XHCI_PORT_REGS_SIZE) {
        size_t port_id = (offset - XHCI_PORT_REGS_BASE) >> 4;
        size_t port_off = (offset & 0xC);
        val = xhci_port_reg_read(xhci, port_id, port_off);
    } else if (offset >= XHCI_EXT_CAPS_BASE) {
        size_t entry = (offset - XHCI_EXT_CAPS_BASE) >> 2;
        if (entry < STATIC_ARRAY_SIZE(xhci_ext_caps)) {
            val = xhci_ext_caps[entry];
        }
    } else switch (offset) {
        // Capability registers
        case XHCI_REG_CAPLENGTH_HCIVERSION:
            val = XHCI_CAPLEN_HCIVERSION;
            break;
        case XHCI_REG_HCSPARAMS1:
            val = XHCI_HCSPARAMS1;
            break;
        case XHCI_REG_HCCPARAMS1:
            val = XHCI_HCCPARAMS1;
            break;
        case XHCI_REG_DBOFF:
            val = XHCI_DOORBELL_BASE;
            break;
        case XHCI_REG_RTSOFF:
            val = XHCI_RUNTIME_BASE;
            break;

        // Operational registers
        case XHCI_REG_USBCMD:
            val = xhci->usbcmd;
            break;
        case XHCI_REG_USBSTS:
            val = ((~xhci->usbcmd) & XHCI_USBSTS_HCH);
            break;
        case XHCI_REG_PAGESIZE:
            val = 0x1; // 4k pages
            break;
        case XHCI_REG_DNCTRL:
            val = xhci->dnctrl;
            break;

        default:
            //rvvm_warn("xhci read  %08x from %04x", val, (uint32_t)offset);
            break;
    }

    write_uint32_le(data, val);

    return true;
}

static bool xhci_pci_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    xhci_bus_t* xhci = dev->data;
    uint32_t val = read_uint32_le(data);
    UNUSED(size);

    switch (offset) {
        // Operational registers
        case XHCI_REG_USBCMD:
            xhci->usbcmd = val;
            if (xhci->usbcmd & XHCI_USBCMD_HCRST) {
                // Perform controller reset
                xhci->usbcmd &= ~XHCI_USBCMD_HCRST;
            }
            break;
        case XHCI_REG_DNCTRL:
            xhci->dnctrl = val;
            break;

        default:
            //rvvm_warn("xhci write %08x to   %04x", val, (uint32_t)offset);
            break;
    }
    return true;
}

static rvvm_mmio_type_t xhci_type = {
    .name = "xhci",
};

PUBLIC pci_dev_t* usb_xhci_init(pci_bus_t* pci_bus)
{
    xhci_bus_t* xhci = safe_new_obj(xhci_bus_t);
    pci_dev_desc_t xhci_desc = {
        .func[0] = {
            .vendor_id = 0x100b,  // National Semiconductor Corporation
            .device_id = 0x0012,  // USB Controller
            .class_code = 0x0C03, // Serial bus controller, USB controller
            .prog_if = 0x30,      // XHCI
            .irq_pin = PCI_IRQ_PIN_INTA,
            .bar[0] = {
                .addr = PCI_BAR_ADDR_64,
                .size = XHCI_BAR_SIZE,
                .min_op_size = 4,
                .max_op_size = 4,
                .read = xhci_pci_read,
                .write = xhci_pci_write,
                .data = xhci,
                .type = &xhci_type,
            }
        }
    };

    pci_dev_t* pci_dev = pci_bus_add_device(pci_bus, &xhci_desc);
    if (pci_dev) xhci->pci_dev = pci_dev;
    return pci_dev;
}
