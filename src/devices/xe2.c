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
#include <stdint.h>

// MCR (Multicast/Replicated)
// MTL (Meteor Lake)

// Now:
//
// static int
// intel_dp_aux_xfer(struct intel_dp *intel_dp,
// 		  const u8 *send, int send_bytes,
// 		  u8 *recv, int recv_size,
// 		  u32 aux_send_ctl_flags)
//
// [  355.414977] xe 0000:00:01.0: [drm] *ERROR* AUX A/DDI A/PHY A: not done (status 0x00000000)

#define xe2_reg_genmask(h, l)           (((~0u) << (l)) & (~0u >> (31 - (h))))
#define xe2_reg_field_get(mask, val)    (((val) & (mask)) >> __builtin_ctz(mask))
#define xe2_reg_field_prep(mask, val)   (((val) << __builtin_ctz(mask)) & (mask))

#define XE2_VENDOR_ID_INTEL                         0x8086
#define XE2_DEVICE_ID_LUNAR_LAKE_IGPU               0x6420
#define XE2_CLASS_CODE                              0x0300

#define XE2_REG_GT_GMD_ID                           0xD8C
#define XE2_REG_GT_GMD_ID_ARCH_MASK                 xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_RELEASE_MASK              xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_REVID_MASK                xe2_reg_genmask(5, 0)

#define XE2_REG_GT_GMD_ID_DISPLAY                   0x510A0
#define XE2_REG_GT_GMD_ID_DISPLAY_ARCH_MASK         xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_DISPLAY_RELEASE_MASK      xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_DISPLAY_REVID_MASK        xe2_reg_genmask(5, 0)

#define XE2_REG_GT_FORCEWAKE_GSC                    0xA618
#define XE2_REG_GT_FORCEWAKE_ACK_GSC                0xDF8
#define XE2_REG_GT_FORCEWAKE_GT                     0xA188
#define XE2_REG_GT_FORCEWAKE_ACK_GT                 0x130044 // How these forcewakes are related?
#define XE2_REG_GT_FORCEWAKE_ACK_GT_MTL             0xDFC

#define XE2_REG_MTL_MEM_SS_INFO                     0x45700 // Memory subsystem configuration
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_MASK     xe2_reg_genmask(11, 8)
#define XE2_REG_MTL_MEM_SS_INFO_N_CHANNELS_MASK     xe2_reg_genmask(7, 4)
#define XE2_REG_MTL_MEM_SS_INFO_DDR_TYPE_MASK       xe2_reg_genmask(3, 0)

#define XE2_REG_STEER_SEMAPHORE                     0xFD0
#define XE2_REG_GGC                                 0x108040 // Graphics control
#define XE2_REG_GGC_GMS_MASK                        xe2_reg_genmask(15, 8)
#define XE2_REG_GGC_GGMS_MASK                       xe2_reg_genmask(7, 6)

#define XE2_REG_GUC_TLB_INV_DESC0                   0xCF7C
#define XE2_REG_GUC_TLB_INV_DESC1                   0xCF80
#define XE2_REG_GU_CNTL_PROTECTED                   0x10100C // This being used from i915
#define XE2_REG_GU_CNTL_PROTECTED_PRESENT_MASK      xe2_reg_genmask(9, 9)

#define XE2_REG_VF_CAP                              0x1901F8
#define XE2_REG_VF_CAP_MASK                         xe2_reg_genmask(0, 0)

#define XE2_DPA_AUX_CH_CTL                          0x64010 // i915/display/intel_dp_aux_regs.h
#define XE2_DPB_AUX_CH_CTL                          0x64110

typedef struct {
    pci_func_t *pci_func;
    uint32_t    forcewake_gsc;
    uint32_t    forcewake_gt_mtl;
    // I don't understand now what exactly this semaphore
    // shall lock.
    uint32_t    steer_semaphore;
} xe2_dev_t;

static void xe2_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t xe2_type = {
    .name = "xe2",
    .remove = xe2_remove,
};

static bool xe2_mmio_read(rvvm_mmio_dev_t *dev, void *data, size_t offset, uint8_t size)
{
    UNUSED(size);
    rvvm_info("PCI read: offset=%lx, data=%x", offset, read_uint32_le(data));

    xe2_dev_t *xe2 = dev->data;

    switch (offset) {
        case XE2_REG_GT_GMD_ID: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GT_GMD_ID_ARCH_MASK, 20)
                         | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_RELEASE_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_REVID_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GT_GMD_ID_DISPLAY: {
            // i915/display/intel_display_device.c:
            // static const struct {
            //      u16 ver;
            //      u16 rel;
            //      const struct intel_display_device_info *display;
            //  gmdid_display_map[] = {
            //      { 14,  0, &xe_lpdp_display },
            //      { 14,  1, &xe2_hpd_display },
            //      { 20,  0, &xe2_lpd_display },
            //      { 30,  0, &xe2_lpd_display },
            //      { 30,  2, &xe2_lpd_display },
            // };
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GT_GMD_ID_DISPLAY_ARCH_MASK, 14)
                         | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_DISPLAY_RELEASE_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_DISPLAY_REVID_MASK, 0);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GT_FORCEWAKE_ACK_GSC:
            write_uint32_le(data, xe2->forcewake_gsc);
            break;
        case XE2_REG_GT_FORCEWAKE_ACK_GT_MTL:
        case XE2_REG_GT_FORCEWAKE_ACK_GT:
            write_uint32_le(data, xe2->forcewake_gt_mtl);
            break;
        case XE2_REG_GGC: {
	        // GGMS should be fixed 0x3 (8MB), which corresponds to the GTT size.
            // GMS has range 0x00 ... 0x04, 0xF0 ... 0xFE according to Linux.
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GGC_GGMS_MASK, 3)
                         | xe2_reg_field_prep(XE2_REG_GGC_GMS_MASK, 4);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_VF_CAP:
            // We don't support SR-IOV.
            write_uint32_le(data, 0);
            break;
        case XE2_REG_MTL_MEM_SS_INFO: {
            // DDR type:   xelpdp_get_dram_info()
            // N channels: DRAM channels number
            // QGV:        I don't know what the fuck is this
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_DDR_TYPE_MASK, 2)
                         | xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_N_CHANNELS_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GU_CNTL_PROTECTED: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GU_CNTL_PROTECTED_PRESENT_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_STEER_SEMAPHORE:
            write_uint32_le(data, xe2->steer_semaphore);
            break;
        default:
            break;
    }

    return true;
}

static bool xe2_mmio_write(rvvm_mmio_dev_t *dev, void *data, size_t offset, uint8_t size)
{
    UNUSED(size);
    rvvm_info("PCI write: offset=%lx, data=%x", offset, read_uint32_le(data));

    xe2_dev_t *xe2 = dev->data;

    switch (offset) {
        case XE2_REG_GT_FORCEWAKE_GSC:
            xe2->forcewake_gsc = read_uint32_le(data);
            break;
        case XE2_REG_GT_FORCEWAKE_GT:
            xe2->forcewake_gt_mtl = read_uint32_le(data);
            break;
        case XE2_REG_STEER_SEMAPHORE:
            xe2->steer_semaphore = read_uint32_le(data);
            break;
        default:
            break;
    }

    return true;
}

PUBLIC pci_dev_t *xe2_init(pci_bus_t *pci_bus)
{
    xe2_dev_t *xe2 = safe_new_obj(xe2_dev_t);
    xe2->forcewake_gsc = 0;
    xe2->forcewake_gt_mtl = 0;
    xe2->steer_semaphore = 1; // Begin with unlocked state.

    pci_func_desc_t xe2_desc = {
        .vendor_id  = XE2_VENDOR_ID_INTEL,
        .device_id  = XE2_DEVICE_ID_LUNAR_LAKE_IGPU,
        .class_code = XE2_CLASS_CODE,
        .prog_if    = 0,
        .irq_pin    = PCI_IRQ_PIN_INTA,
        // MMIO + GTT
        .bar[0]     = {
            .size           = 0x10000000,
            .min_op_size    = 1,
            .max_op_size    = 4,
            .read           = xe2_mmio_read,
            .write          = xe2_mmio_write,
            .data           = xe2,
            .type           = &xe2_type
        },
        // VRAM
        .bar[2]         = {
            .size           = 0x10000000,
            .min_op_size    = 1,
            .max_op_size    = 4,
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

// Эти портреты безлики, он написал их
// На чёрном холсте
// Безобразным движением кисти
// Эти картины тревожны, и он их прятал во тьме
// Неужели он был художник?
// Все узоры пропитаны горем
// В болезненной форме
// Они снова берутся за краски
// Однотонные мрачные краски
// Они вместе рисуют смерть
// Монохромом на чёрном холсте
