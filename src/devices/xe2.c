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

#define xe2_reg_genmask(h, l)           (((~0U) << (l)) & (~0U >> (31 - (h))))
#define xe2_reg_bit(x)                  xe2_reg_genmask((x), (x))
#define xe2_reg_field_get(mask, val)    (((val) & (mask)) >> __builtin_ctz(mask))
#define xe2_reg_field_prep(mask, val)   (((val) << __builtin_ctz(mask)) & (mask))

#define XE2_VENDOR_ID_INTEL                             0x8086
#define XE2_DEVICE_ID_LUNAR_LAKE_IGPU                   0x6420
#define XE2_CLASS_CODE                                  0x0300

#define XE2_REG_GT_GMD_ID                               0xD8C
#define XE2_REG_GT_GMD_ID_ARCH_MASK                     xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_RELEASE_MASK                  xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_REVID_MASK                    xe2_reg_genmask(5, 0)

#define XE2_REG_GT_GMD_ID_DISPLAY                       0x510A0
#define XE2_REG_GT_GMD_ID_DISPLAY_ARCH_MASK             xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_DISPLAY_RELEASE_MASK          xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_DISPLAY_REVID_MASK            xe2_reg_genmask(5, 0)

#define XE2_REG_GT_FORCEWAKE_GSC                        0xA618
#define XE2_REG_GT_FORCEWAKE_ACK_GSC                    0xDF8
#define XE2_REG_GT_FORCEWAKE_GT                         0xA188
#define XE2_REG_GT_FORCEWAKE_ACK_GT                     0x130044 // How these forcewakes are related?
#define XE2_REG_GT_FORCEWAKE_ACK_GT_MTL                 0xDFC

#define XE2_REG_MTL_MEM_SS_INFO                         0x45700 // Memory subsystem configuration
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_MASK         xe2_reg_genmask(11, 8)
#define XE2_REG_MTL_MEM_SS_INFO_N_CHANNELS_MASK         xe2_reg_genmask(7, 4)
#define XE2_REG_MTL_MEM_SS_INFO_DDR_TYPE_MASK           xe2_reg_genmask(3, 0)

#define XE2_REG_STEER_SEMAPHORE                         0xFD0
#define XE2_REG_GGC                                     0x108040 // Graphics control
#define XE2_REG_GGC_GMS_MASK                            xe2_reg_genmask(15, 8)
#define XE2_REG_GGC_GGMS_MASK                           xe2_reg_genmask(7, 6)

#define XE2_REG_GUC_TLB_INV_DESC0                       0xCF7C
#define XE2_REG_GUC_TLB_INV_DESC1                       0xCF80
#define XE2_REG_GU_CNTL_PROTECTED                       0x10100C // This being used from i915
#define XE2_REG_GU_CNTL_PROTECTED_PRESENT_MASK          xe2_reg_genmask(9, 9)

#define XE2_REG_VF_CAP                                  0x1901F8
#define XE2_REG_VF_CAP_MASK                             xe2_reg_genmask(0, 0)

#define XE2_REG_DPA_AUX_CH_CTL                          0x64010 // i915/display/intel_dp_aux_regs.h
#define XE2_REG_DPB_AUX_CH_CTL                          0x64110
#define XE2_REG_DBX_AUX_CH_CTL_SEND_BUSY_MASK           xe2_reg_bit(31)
#define XE2_REG_DBX_AUX_CH_CTL_DONE_MASK                xe2_reg_bit(30)
#define XE2_REG_DBX_AUX_CH_CTL_INTERRUPT_MASK           xe2_reg_bit(29)
#define XE2_REG_DBX_AUX_CH_CTL_TIME_OUT_ERROR_MASK      xe2_reg_bit(28)
#define XE2_REG_DBX_AUX_CH_CTL_TIME_OUT_MASK            xe2_reg_genmask(27, 26)
#define XE2_REG_DBX_AUX_CH_CTL_RECEIVE_ERROR_MASK       xe2_reg_bit(25)
#define XE2_REG_DBX_AUX_CH_CTL_MSG_SIZE_MASK            xe2_reg_genmask(24, 20)
#define XE2_REG_DBX_AUX_CH_CTL_PRECHARGE_2US_MASK       xe2_reg_genmask(19, 16)
#define XE2_REG_DBX_AUX_CH_CTL_AUX_AKSV_SELECT_MASK     xe2_reg_bit(15)
#define XE2_REG_DBX_AUX_CH_CTL_MANCHESTER_MASK          xe2_reg_bit(14)
#define XE2_REG_DBX_AUX_CH_CTL_PSR_DATA_AUX_SKL_MASK    xe2_reg_bit(14)
#define XE2_REG_DBX_AUX_CH_CTL_SYNC_TEST_MASK           xe2_reg_bit(13)
#define XE2_REG_DBX_AUX_CH_CTL_FS_DATA_AUX_SKL_MASK     xe2_reg_bit(13)
#define XE2_REG_DBX_AUX_CH_CTL_DEGLITCH_TEST__MASK      xe2_reg_bit(12)
#define XE2_REG_DBX_AUX_CH_CTL_GTC_DATA_AUX_REG_MASK    xe2_reg_bit(12)
#define XE2_REG_DBX_AUX_CH_CTL_PRECHARGE_TEST_MASK      xe2_reg_bit(11)
#define XE2_REG_DBX_AUX_CH_CTL_TBT_IO_MASK              xe2_reg_bit(11)
#define XE2_REG_DBX_AUX_CH_CTL_BIT_CLOCK_2X_MASK        xe2_reg_genmask(10, 0)
#define XE2_REG_DBX_AUX_CH_CTL_FW_SYNC_PULSE_SKL_MASK   xe2_reg_genmask(9, 5)
#define XE2_REG_DBX_AUX_CH_CTL_SYNC_PUSLE_SKL_MASK      xe2_reg_genmask(4, 0)

#define XE2_REG_PP_STATUS                               0x61200 // Panel power sequence
#define XE2_REG_PP_ON_MASK                              xe2_reg_bit(31)
#define XE2_REG_PP_READY_MASK                           xe2_reg_bit(30)
#define XE2_REG_PP_SEQUENCE_MASK                        xe2_reg_genmask(29, 28)
#define XE2_REG_PP_CYCLE_DELAY_ACTIVE_MASK              xe2_reg_bit(27)

#define XE2_REG_PP_CONTROL                              0x61204
#define XE2_REG_PP_CONTROL_UNLOCK_MASK                  xe2_reg_genmask(31, 16)
#define XE2_REG_PP_CONTROL_POWER_CYCLE_DELAY_MASK       xe2_reg_genmask(8, 4)
#define XE2_REG_PP_CONTROL_EPD_FORCE_VDD_MASK           xe2_reg_bit(3)
#define XE2_REG_PP_CONTROL_EPD_BLC_ENABLE_MASK          xe2_reg_bit(2)
#define XE2_REG_PP_CONTROL_POWER_RESET_MASK             xe2_reg_bit(1)
#define XE2_REG_PP_CONTROL_POWER_ON_MASK                xe2_reg_bit(0)

#define XE2_REG_HSW_POWER_WELL_CTL1                     0x45400
#define XE2_REG_HSW_POWER_WELL_CTL2                     0x45404
#define XE2_REG_HSW_POWER_WELL_CTL3                     0x45408
#define XE2_REG_HSW_POWER_WELL_CTL4                     0x4540C

#define XE2_REG_BXT_DE_PLL_ENABLE                       0x46070
#define XE2_REG_BXT_DE_PLL_ENABLE_MASK                  xe2_reg_bit(31)
#define XE2_REG_BXT_DE_PLL_ENABLE_LOCK_MASK             xe2_reg_bit(30)
#define XE2_REG_BXT_DE_PLL_ENABLE_FREQ_REQ_MASK         xe2_reg_bit(23)
#define XE2_REG_BXT_DE_PLL_ENABLE_FREQ_REQ_ACK_MASK     xe2_reg_bit(22)

#define XE2_REG_SKL_FUSE_STATUS                         0x42000
#define XE2_REG_SKL_FUSE_STATUS_DOWNLOAD_MASK           xe2_reg_bit(31)
// Power gates:
//   SKL_PG0 = 0
//   SKL_PG1 = 1
//   SKL_PG2 = 2
//   ICL_PG3 = 3
//   ICL_PG4 = 4
#define XE2_REG_SKL_FUSE_STATUS_DST_MASK(pg)            (1 << (27 - (pg)))

#define XE2_REG_DBUF_CTL_S0                                 0x45008
#define XE2_REG_DBUF_CTL_S1                                 0x44FE8
#define XE2_REG_DBUF_CTL_S2                                 0x44300
#define XE2_REG_DBUF_CTL_S3                                 0x44304
#define XE2_REG_DBUF_CTL_SX_POWER_REQUEST_MASK              xe2_reg_bit(31)
#define XE2_REG_DBUF_CTL_SX_POWER_STATE_MASK                xe2_reg_bit(30)
#define XE2_REG_DBUF_CTL_SX_TRACKER_STATE_SERVICE_MASK      xe2_reg_genmask(23, 19)
#define XE2_REG_DBUF_CTL_SX_MIN_TRACKER_STATE_SERVICE_MASK  xe2_reg_genmask(18, 16)

#define XE2_REG_CDCLK_CTL                                   0x46000
#define XE2_REG_CDCLK_CTL_FREQ_SEL_MASK                     xe2_reg_genmask(27, 26)
#define XE2_REG_CDCLK_CTL_SOURCE_SEL_MASK                   xe2_reg_bit(25)
#define XE2_REG_CDCLK_CTL_CD2X_DIV_SEL_MASK                 xe2_reg_genmask(23, 22)
#define XE2_REG_CDCLK_CTL_CD2X_PIPE_MASK                    xe2_reg_bit(20)

typedef struct {
    pci_func_t *pci_func;
    uint32_t    forcewake_gsc;
    uint32_t    forcewake_gt_mtl;
    uint32_t    pll_enable;
    uint32_t    dbuf_ctl[4];
    uint32_t    pp_control;
    // I don't understand now what exactly this semaphore
    // shall lock.
    uint32_t    steer_semaphore;
    struct {
        uint8_t power_request;
        uint32_t data[5];
        uint32_t ctl;
    } aux;
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
        case XE2_REG_PP_STATUS: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_PP_ON_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_PP_READY_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_PP_CONTROL: {
            uint32_t cmd = xe2->pp_control;
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_HSW_POWER_WELL_CTL1:
        case XE2_REG_HSW_POWER_WELL_CTL2:
        case XE2_REG_HSW_POWER_WELL_CTL3:
        case XE2_REG_HSW_POWER_WELL_CTL4:
            write_uint32_le(data, 0xFFFFFFFF);
            break;
        case XE2_REG_BXT_DE_PLL_ENABLE: {
            xe2->pll_enable &= ~xe2_reg_field_prep(XE2_REG_BXT_DE_PLL_ENABLE_MASK, 1);
            write_uint32_le(data, xe2->pll_enable);
            break;
        }
        case XE2_REG_SKL_FUSE_STATUS: {
            // According to Linux kernel we have 5 power gates, but driver
            // continues with initialization only when 9 power gates is set.
            // Maybe, XE2 added more power gates compared to i915.
            //
            // enum skl_power_gate {
            //      SKL_PG0,
            //      SKL_PG1,
            //      SKL_PG2,
            //      ICL_PG3,
            //      ICL_PG4,
            // };
            //
            // #define SKL_FUSE_STATUS                  _MMIO(0x42000)
            // #define  SKL_FUSE_DOWNLOAD_STATUS        (1 << 31)
            // #define  SKL_FUSE_PG_DIST_STATUS(pg)     (1 << (27 - (pg)))
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DOWNLOAD_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(0), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(1), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(2), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(3), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(4), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(5), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(6), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(7), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(8), 1)
                         | xe2_reg_field_prep(XE2_REG_SKL_FUSE_STATUS_DST_MASK(9), 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_DBUF_CTL_S0: {
            uint32_t cmd = xe2->dbuf_ctl[0] | xe2_reg_field_prep(XE2_REG_DBUF_CTL_SX_POWER_STATE_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_DBUF_CTL_S1: {
            uint32_t cmd = xe2->dbuf_ctl[1] | xe2_reg_field_prep(XE2_REG_DBUF_CTL_SX_POWER_STATE_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_DBUF_CTL_S2: {
            uint32_t cmd = xe2->dbuf_ctl[2] | xe2_reg_field_prep(XE2_REG_DBUF_CTL_SX_POWER_STATE_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_DBUF_CTL_S3: {
            uint32_t cmd = xe2->dbuf_ctl[3] | xe2_reg_field_prep(XE2_REG_DBUF_CTL_SX_POWER_STATE_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
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
        case XE2_REG_CDCLK_CTL: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_CDCLK_CTL_FREQ_SEL_MASK, 2)
                         | xe2_reg_field_prep(XE2_REG_CDCLK_CTL_SOURCE_SEL_MASK, 0)
                         | xe2_reg_field_prep(XE2_REG_CDCLK_CTL_CD2X_DIV_SEL_MASK, 0)
                         | xe2_reg_field_prep(XE2_REG_CDCLK_CTL_CD2X_PIPE_MASK, 0);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_DPA_AUX_CH_CTL:
        case XE2_REG_DPB_AUX_CH_CTL:
            write_uint32_le(data, xe2->aux.ctl);
            break;
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
        case XE2_REG_DPA_AUX_CH_CTL:
        case XE2_REG_DPB_AUX_CH_CTL: {
            uint32_t cmd = read_uint32_le(data);
            xe2->aux.power_request = (cmd >> 19) & 1;
            if (cmd & (1U << 31)) {
                xe2->aux.data[0] = 0x00000014;
                xe2->aux.data[1] = 0x00000001;
                xe2->aux.data[2] = 0x00000084;
                uint32_t out_cmd = cmd;
                out_cmd &=  ~(1U << 31);
                out_cmd |=   (1U << 30);
                out_cmd |=   (1U << 18);
                out_cmd &= ~((1U << 28) | (1U << 25));
                write_uint32_le(data, out_cmd);
                xe2->aux.ctl = out_cmd;
            } else {
                write_uint32_le(data, cmd | (1U << 18));
            }
            break;
        }
        case XE2_REG_BXT_DE_PLL_ENABLE:
            xe2->pll_enable = read_uint32_le(data);
            break;
        case XE2_REG_PP_CONTROL:
            xe2->pp_control = read_uint32_le(data);
            break;
        case XE2_REG_DBUF_CTL_S0:
            xe2->dbuf_ctl[0] = read_uint32_le(data);
            break;
        case XE2_REG_DBUF_CTL_S1:
            xe2->dbuf_ctl[1] = read_uint32_le(data);
            break;
        case XE2_REG_DBUF_CTL_S2:
            xe2->dbuf_ctl[2] = read_uint32_le(data);
            break;
        case XE2_REG_DBUF_CTL_S3:
            xe2->dbuf_ctl[3] = read_uint32_le(data);
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
