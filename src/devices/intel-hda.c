/*
intel-hda.h - Intel High Definition Audio
Copyright (C) 2025  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "intel-hda.h"
#include "compiler.h"
#include "mem_ops.h"
#include "utils.h"

// CORB - Command Outbound Ring Buffer
// https://github.com/VendelinSlezak/BleskOS/blob/master/source/drivers/sound/hda.c

#define INTEL_HDA_GCAP                0x00 // Global capabilities
#define INTEL_HDA_VS                  0x02 // 0x02 minor, 0x03 major
#define INTEL_HDA_OUTPAY              0x04 // Output payload capability
#define INTEL_HDA_INPAY               0x06 // Input payload capability
#define INTEL_HDA_GLOBAL_CTRL         0x08 // Global Control
#define INTEL_HDA_INTR_CTRL           0x20 // Interrupt Control
#define INTEL_HDA_STREAM_SYNC         0x34 // Stream Synchronization
#define INTEL_HDA_CORB_LO             0x40 // CORB Lower Base Address
#define INTEL_HDA_CORB_HI             0x44 // CORB Upper Base Address
#define INTEL_HDA_CORB_WP             0x48 // CORB Write Pointer
#define INTEL_HDA_CORB_RP             0x4A // CORB Read Pointer
#define INTEL_HDA_CORB_CTRL           0x4C // CORB Control
#define INTEL_HDA_CORB_SIZE           0x4E // CORB Size
#define INTEL_HDA_RIRB_LO             0x50 // RIRB Lower Base Address
#define INTEL_HDA_RIRB_HI             0x54 // RIRB Upper Base Address
#define INTEL_HDA_RIRB_WP             0x58 // RIRB Write Pointer
#define INTEL_HDA_RIRB_INTR_CNT       0x5A // RIRB Response Interrupt Count
#define INTEL_HDA_RIRB_CTRL           0x5C // RIRB Control
#define INTEL_HDA_RIRB_SIZE           0x5E // RIRB Size
#define INTEL_HDA_DMA_LO              0x70 // DMA Position Lower Base Address
#define INTEL_HDA_DMA_HI              0x74 // DMA Position Upper Base Address

#define INTEL_HDA_PARAM_V             0x103 // Version 1.03
#define INTEL_HDA_PARAM_NO_OUT        0x01  // Number of output streams supported
#define INTEL_HDA_PARAM_NO_IN         0x01  // Number of input streams supported
#define INTEL_HDA_PARAM_NO_BSS        0x00  // Number of bidirectional streams supported
#define INTEL_HDA_PARAM_NO_NSDO       0x00  // Number of serial data out signals
#define INTEL_HDA_PARAM_64_BIT        0x01  // 64-bit support

#define INTEL_HDA_PARAM_GCAP          ((INTEL_HDA_PARAM_NO_OUT  & 15) << 12) \
                                    | ((INTEL_HDA_PARAM_NO_IN   & 15) <<  8) \
                                    | ((INTEL_HDA_PARAM_NO_BSS  & 31) <<  3) \
                                    | ((INTEL_HDA_PARAM_NO_NSDO &  2) <<  1) \
                                    | ((INTEL_HDA_PARAM_64_BIT  &  1))

typedef struct {
    pci_func_t* pci_func;
} intel_hda_dev_t;

static void intel_hda_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t hda_type = {
    .name = "intel-hda",
    .remove = intel_hda_remove,
};

static bool intel_hda_mmio_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);

    // intel_hda_dev_t *hda = dev->data;

    switch (offset) {
        case INTEL_HDA_GCAP:
            write_uint16_le(data, INTEL_HDA_PARAM_GCAP);
            break;
        case INTEL_HDA_VS:
            write_uint16_le(data, INTEL_HDA_PARAM_V);
            break;
        case INTEL_HDA_GLOBAL_CTRL:
            // Notify if reset is completed?
            break;
        case INTEL_HDA_INTR_CTRL: {
            uint32_t intr_ctrl = (1 << 31) | (1 << 30);
            rvvm_info("Intel HDA intr ctrl: %08x", intr_ctrl);
            write_uint32_le(data, intr_ctrl);
            break;
        }
        case INTEL_HDA_STREAM_SYNC:
            break;
        case INTEL_HDA_CORB_LO:
            rvvm_info("Intel HDA CORB lo: %x", read_uint32_le(data));
            break;
        case INTEL_HDA_CORB_HI:
            rvvm_info("Intel HDA CORB hi: %x", read_uint32_le(data));
            break;
        case INTEL_HDA_CORB_WP:
            break;
        case INTEL_HDA_CORB_RP:
            break;
        case INTEL_HDA_CORB_CTRL:
            break;
        case INTEL_HDA_CORB_SIZE:
            break;
        case INTEL_HDA_RIRB_LO:
            break;
        case INTEL_HDA_RIRB_HI:
            break;
        case INTEL_HDA_RIRB_WP:
            break;
        case INTEL_HDA_RIRB_INTR_CNT:
            break;
        case INTEL_HDA_RIRB_CTRL:
            break;
        case INTEL_HDA_RIRB_SIZE:
            break;
        case INTEL_HDA_DMA_LO:
            break;
        case INTEL_HDA_DMA_HI:
            break;
        default:
            break;
    }

    return true;
}

static bool intel_hda_mmio_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);

    switch (offset) {
        case INTEL_HDA_GLOBAL_CTRL: {
            uint32_t cmd = read_uint32_le(data);

            // Writing a 0 to this bit causes the High Definition Audio
            // controller to transition to the Reset state.
            //
            // After the hardware has completed sequencing
            // into the reset state, it will report a 0 in this bit.
            // Software must read a 0 from this bit
            // to verify that the controller is in reset.
            if (!(cmd & 1))
                rvvm_info("Intel HDA global ctrl: controller reset requested");

            if (!(cmd & 2))
                rvvm_info("Intel HDA global ctrl: flush control");

            break;
        }
        case INTEL_HDA_INTR_CTRL:
            rvvm_info("Intel HDA interrupt ctrl: %x", read_uint32_le(data));
            break;
        case INTEL_HDA_DMA_LO:
            rvvm_info("Intel HDA DMA lower: %x", read_uint32_le(data));
            break;
        case INTEL_HDA_DMA_HI:
            rvvm_info("Intel HDA DMA upper: %x", read_uint32_le(data));
            break;
        case INTEL_HDA_CORB_CTRL: {
            uint8_t cmd = read_uint8(data);
            if (cmd & 2)
                rvvm_info("Intel CORB run");
            else
                rvvm_info("Intel CORB stop");
            break;
        }
        case INTEL_HDA_RIRB_CTRL: {
            uint8_t cmd = read_uint8(data);
            if (cmd & 2)
                rvvm_info("Intel RIRB run");
            else
                rvvm_info("Intel RIRB stop");
            break;
        }
        case INTEL_HDA_STREAM_SYNC: {
            uint32_t cmd = read_uint32_le(data);
            if (cmd == 0)
                rvvm_info("Intel HDA stream sync disable");
            else
                rvvm_info("Intel HDA stream sync: %x", cmd);
            break;
        }
        default:
            return false;
    }

    return true;
}

// [    0.180451] pci 0000:00:02.0: [8086:2668] type 00 class 0x040300 PCIe Root Complex Integrated Endpoint
// [    0.180505] pci 0000:00:02.0: BAR 0 [mem 0x40002000-0x40002fff 64bit]
// [    0.345685] snd_hda_intel 0000:00:02.0: Force to snoop mode by module option
//                ^
//                sound/pci/hda/hda_intel.c:1689
PUBLIC pci_dev_t* intel_hda_init(pci_bus_t* pci_bus)
{
    intel_hda_dev_t *intel_hda = safe_new_obj(intel_hda_dev_t);

    pci_func_desc_t intel_hda_desc = {
        .vendor_id  = 0x8086,
        .device_id  = 0x2668,
        .class_code = 0x0403,
        .prog_if    = 0x00,
        .irq_pin    = PCI_IRQ_PIN_INTA,
        .bar[0] = {
            .size        = 0x1000,
            .min_op_size = 4,
            .max_op_size = 4,
            .read        = intel_hda_mmio_read,
            .write       = intel_hda_mmio_write,
            .data        = intel_hda,
            .type        = &hda_type
        },
    };

    pci_dev_t* pci_dev = pci_attach_func(pci_bus, &intel_hda_desc);
    if (pci_dev) {
        // Successfully plugged in
        intel_hda->pci_func = pci_get_device_func(pci_dev, 0);
    }

    return pci_dev;
}

PUBLIC pci_dev_t* intel_hda_init_auto(rvvm_machine_t* machine)
{
    return intel_hda_init(rvvm_get_pci_bus(machine));
}