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

// Capability registers
#define XHCI_REG_CAPLENGTH_HCIVERSION 0x00 // Capability Register Length (byte 0), Interface Version Number (byte 2)
#define XHCI_REG_HCSPARAMS1           0x04 // Structural Parameters 1
#define XHCI_REG_HCSPARAMS2           0x08 // Structural Parameters 2
#define XHCI_REG_HCSPARAMS3           0x0C // Structural Parameters 3
#define XHCI_REG_HCCPARAMS1           0x10 // Capability Parameters
#define XHCI_REG_DBOFF                0x14 // Doorbell Offset
#define XHCI_REG_RTSOFF               0x18 // Runtime Registers Space Offset
#define XHCI_REG_HCCPARMS2            0x1C // Capability Parameters 2

// Host controller capability constants
#define XHCI_OPERATIONAL_BASE 0x80
#define XHCI_EXT_CAPS_BASE 0x8000
#define XHCI_VERSION 0x0100 // XHCI v1.0.0

#define XHCI_MAX_SLOTS 0x20 // 32 slots max
#define XHCI_MAX_PORTS 0x20 // 32 ports max
#define XHCI_MAX_IRQS  0x01 // 1 interrupter

#define XHCI_CAPLEN_HCIVERSION (XHCI_OPERATIONAL_BASE | (XHCI_VERSION << 16))
#define XHCI_HCSPARAMS1        (XHCI_MAX_SLOTS | (XHCI_MAX_IRQS << 8) | (XHCI_MAX_PORTS << 24))

#define XHCI_HCCPARAMS1 ((XHCI_EXT_CAPS_BASE << 14) | 0x5) // 64-bit addressing, 64-byte context size

// Operational registers
#define XHCI_REG_USBCMD   0x80 // USB Command
#define XHCI_REG_USBSTS   0x84 // USB Status
#define XHCI_REG_PAGESIZE 0x88 // Page Size
#define XHCI_REG_DNCTRL   0x94 // Device Notification Control
#define XHCI_REG_CRCR     0x98 // Command Ring Control
#define XHCI_REG_CRCR_H   0x9C
#define XHCI_REG_DCBAAP   0xB0 // Device Context Base Address Array Pointer
#define XHCI_REG_CONFIG   0xB8 // Configure

// USB Command values
#define XHCI_USBCMD_RS    0x001 // Run/Stop
#define XHCI_USBCMD_HCRST 0x002 // Host Controller Reset
#define XHCI_USBCMD_INTE  0x004 // Interrupter Enable

// USB Status values
#define XHCI_USBSTS_HCH  0x001 // HCHalted
#define XHCI_USBSTS_EINT 0x008 // Event interrupt
#define XHCI_USBSTS_PCD  0x010 // Port Change Detected
#define XHCI_USBSTS_CNR  0x800 // Controller Not Ready

// Port registers
#define XHCI_PORT_REGS_OFFSET 0x400
#define XHCI_PORT_REGS_SIZE   0x10

#define XHCI_REG_PORTSC   0x0 // Port Status and Control
#define XHCI_REG_PORTPMSC 0x4 // Port Power Management Status and Control
#define XHCI_REG_PORTLI   0x8 // Port Link Info

// Runtime registers
#define XHCI_RUNTIME_REGS_OFFSET 0x2000

// Doorbell registers
#define XHCI_DOORBELL_REGS_OFFSET 0x3000

typedef struct {
    pci_dev_t* pci_dev;

    uint32_t usbcmd;
    uint32_t dnctrl;
} xhci_bus_t;

static bool xhci_pci_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    xhci_bus_t* xhci = dev->data;
    uint32_t val = 0;
    UNUSED(size);

    switch (offset) {
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
            val = XHCI_DOORBELL_REGS_OFFSET;
            break;
        case XHCI_REG_RTSOFF:
            val = XHCI_RUNTIME_REGS_OFFSET;
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

        case XHCI_EXT_CAPS_BASE:
            val = 0x02000002;
            break;
        case XHCI_EXT_CAPS_BASE + 8:
            val = 0x0200101;
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
                .size = 0x10000,
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
