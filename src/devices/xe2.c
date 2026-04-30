/*
xe2.c - Intel XE2 graphics
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "xe2.h"
#include "compiler.h"
#include "devices/pci-bus.h"
#include "mem_ops.h"
#include "utils.h"

#define xe2_reg_genmask(h, l)           (((~0u) << (l)) & (~0u >> (31 - (h))))
#define xe2_reg_field_get(mask, val)    (((val) & (mask)) >> __builtin_ctz(mask))
#define xe2_reg_field_prep(mask, val)   (((val) << __builtin_ctz(mask)) & (mask))

#define XE2_VENDOR_ID_INTEL             0x8086
#define XE2_DEVICE_ID_LUNAR_LAKE_IGPU   0x6420
#define XE2_CLASS_CODE                  0x0300

// 31        21              5    0
// |   arch  |    release    | rev|
// 00000000000000000000000000000000
#define XE2_REG_GT_GMD_ID                   0xD8C
#define XE2_REG_GT_GMD_ID_ARCH_MASK         xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_RELEASE_MASK      xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_REVID_MASK        xe2_reg_genmask(5, 0)

#define XE2_REG_GT_FORCEWAKE_ACK_GSC        0xDF8
#define XE2_REG_GT_FORCEWAKE_ACK_GT_MTL     0xDFC

typedef struct {
    pci_func_t *pci_func;
} xe2_dev_t;

static void xe2_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t xe2_type = {
    .name = "xe2",
    .remove = xe2_remove,
};

static bool xe2_mmio_read(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("PCI read: offset=%lx, data=%x", offset, read_uint32_le(data));

    switch (offset) {
    case XE2_REG_GT_GMD_ID: {
        uint32_t cmd = xe2_reg_field_prep(XE2_REG_GT_GMD_ID_ARCH_MASK, 20)
                     | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_RELEASE_MASK, 1)
                     | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_REVID_MASK, 1);
        write_uint32_le(data, cmd);
        break;
    }
    default:
        break;
    }

    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool xe2_mmio_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("PCI write: offset=%lx, data=%x", offset, read_uint32_le(data));
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

PUBLIC pci_dev_t *xe2_init(pci_bus_t *pci_bus)
{
    xe2_dev_t *xe2 = safe_new_obj(xe2_dev_t);

    pci_func_desc_t xe2_desc = {
        .vendor_id  = XE2_VENDOR_ID_INTEL,
        .device_id  = XE2_DEVICE_ID_LUNAR_LAKE_IGPU,
        .class_code = XE2_CLASS_CODE,
        .prog_if    = 0,
        .irq_pin    = PCI_IRQ_PIN_INTA,
        // MMIO + GTT
        .bar[0]     = {
            .size           = 0x1000000, // 16 MB
            .min_op_size    = 1,
            .max_op_size    = 4,
            .read           = xe2_mmio_read,
            .write          = xe2_mmio_write,
            .data           = xe2,
            .type           = &xe2_type
        },
        // VRAM
        .bar[2]         = {

        }
    };

    pci_dev_t *pci_dev = pci_attach_func(pci_bus, &xe2_desc);
    if (pci_dev)
        xe2->pci_func = pci_get_device_func(pci_dev, 0);

    return pci_dev;
}

PUBLIC pci_dev_t *xe2_init_auto(rvvm_machine_t *machine)
{
    return xe2_init(rvvm_get_pci_bus(machine));
}
