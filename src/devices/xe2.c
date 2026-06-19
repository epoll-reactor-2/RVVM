/*
xe2.c - Intel XE2 graphics
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "xe2.h"
#include "atomics.h"
#include "compiler.h"
#include "devices/pci-bus.h"
#include "mem_ops.h"
#include "rvvm/rvvm_base.h"
#include "spinlock.h"
#include "utils.h"
#include "vma_ops.h"
#include <stdint.h>

// MCR (Multicast/Replicated)
// MTL (Meteor Lake)
//
// Current status: GuC CT communication seems to be correct. Next stage is
// understand messages format and handle it. We deal with same GuC actions
// but another ones for DMA channel, not MMIO one (comments in XE2_REG_GUC_HOST_INTERRUPT).

#define xe2_reg_genmask(h, l)           (((~0U)   << (l)) & (~0U   >> (31 - (h))))
#define xe2_reg_genmask64(h, l)         (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define xe2_reg_bit(x)                  xe2_reg_genmask((x), (x))
#define xe2_reg_field_get(mask, val)    (((val) & (mask)) >> __builtin_ctzll(mask))
#define xe2_reg_field_prep(mask, val)   (((val) << __builtin_ctzll(mask)) & (mask))

#define XE2_VENDOR_ID_INTEL                                 0x8086
#define XE2_DEVICE_ID_ARC_B570_GRAPHICS                     0xE20C
#define XE2_CLASS_CODE                                      0x0300

#define XE2_REG_FLUSH_PENDING                               0x130030 // Dummy register

#define XE2_REG_PCODE_MAILBOX                               0x138124
#define XE2_REG_PCODE_MAILBOX_READY_MASK                    xe2_reg_bit(31)
#define XE2_REG_PCODE_MAILBOX_MB_PARAM2_MASK                xe2_reg_genmask(23, 16)
#define XE2_REG_PCODE_MAILBOX_MB_PARAM1_MASK                xe2_reg_genmask(15,  8)
#define XE2_REG_PCODE_MAILBOX_MB_COMMAND_MASK               xe2_reg_genmask( 7,  0)

#define XE2_REG_PCODE_DATA0                                 0x138128
#define XE2_REG_PCODE_DATA1                                 0x13812C

#define XE2_REG_PCODE_SCRATCH(n)                            (0x138320 + (n) * 4)
#define XE2_REG_PCODE_SCRATCH_AUXINFO_REG_OFFSET_MASK       xe2_reg_genmask(17, 15)
#define XE2_REG_PCODE_SCRATCH_OVERFLOW_REG_OFFSET_MASK      xe2_reg_genmask(14, 12)
#define XE2_REG_PCODE_SCRATCH_HISTORY_TRACKING_MASK         xe2_reg_bit(11)
#define XE2_REG_PCODE_SCRATCH_OVERFLOW_SUPPORT_MASK         xe2_reg_bit(10)
#define XE2_REG_PCODE_SCRATCH_AUXINFO_SUPPORT_MASK          xe2_reg_bit( 9)
#define XE2_REG_PCODE_SCRATCH_BOOT_STATUS_MASK              xe2_reg_genmask(3, 1)

#define XE2_REG_GT_GMD_ID                                   0xD8C
#define XE2_REG_GT_GMD_ID_ARCH_MASK                         xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_RELEASE_MASK                      xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_REVID_MASK                        xe2_reg_genmask(5, 0)

#define XE2_REG_GT_GMD_ID_DISPLAY                           0x510A0
#define XE2_REG_GT_GMD_ID_DISPLAY_ARCH_MASK                 xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_DISPLAY_RELEASE_MASK              xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_DISPLAY_REVID_MASK                xe2_reg_genmask(5, 0)

#define XE2_REG_GU_CNTL                                     0x101010
#define XE2_REG_GU_CNTL_LMEM_INIT_MASK                      xe2_reg_bit(7)
#define XE2_REG_GU_CNTL_DRIVERFLR_MASK                      xe2_reg_bit(31)

#define XE2_REG_PRIMARY_SPI_ADDRESS                         0x102080
#define XE2_REG_PRIMARY_SPI_TRIGGER                         0x102040

#define XE2_REG_GT_FORCEWAKE_GSC                            0xA618
#define XE2_REG_GT_FORCEWAKE_ACK_GSC                        0xDF8
#define XE2_REG_GT_FORCEWAKE_GT                             0xA188
#define XE2_REG_GT_FORCEWAKE_ACK_GT                         0x130044 // How these forcewakes are related?
#define XE2_REG_GT_FORCEWAKE_ACK_GT_MTL                     0xDFC
#define XE2_REG_GT_FORCEWAKE_RENDER                         0xA278
#define XE2_REG_GT_FORCEWAKE_ACK_RENDER                     0xD84

#define XE2_REG_GT_GDRST                                    0x941C
#define XE2_REG_GT_GDRST_GRDOM_GUC                          xe2_reg_bit(3)
#define XE2_REG_GT_GDRST_GRDOM_FULL                         xe2_reg_bit(0)

#define XE2_REG_GT_POWERGATE_ENABLE                         0xA210
#define XE2_REG_GT_POWERGATE_ENABLE_RENDER_MASK             xe2_reg_bit(0)
#define XE2_REG_GT_POWERGATE_ENABLE_MEDIA_MASK              xe2_reg_bit(1)
#define XE2_REG_GT_POWERGATE_ENABLE_MEDIA_SAMPLES_MASK      xe2_reg_bit(2)
#define XE2_REG_GT_POWERGATE_ENABLE_VDN_HCP_MASK(n)         xe2_reg_bit(3 + 2 * (n))
#define XE2_REG_GT_POWERGATE_ENABLE_VDN_MFXVDENC_MASK(n)    xe2_reg_bit(4 + 2 * (n))

#define XE2_REG_MTL_MEM_SS_INFO                             0x45700 // Memory subsystem configuration
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_MASK             xe2_reg_genmask(11, 8)
#define XE2_REG_MTL_MEM_SS_INFO_N_CHANNELS_MASK             xe2_reg_genmask(7, 4)
#define XE2_REG_MTL_MEM_SS_INFO_DDR_TYPE_MASK               xe2_reg_genmask(3, 0)

#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO(n)            (0x45710 + (n) * 8)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_TRCD_MASK     xe2_reg_genmask(31, 24)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_TRP_MASK      xe2_reg_genmask(23, 16)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_DCLK_MASK     xe2_reg_genmask(15,  0)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI(n)            (0x45710 + (n) * 8 + 4)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI_TRAS_MASK     xe2_reg_genmask(16, 8)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI_TRDPRE_MASK   xe2_reg_genmask( 7, 0)

#define XE2_REG_STEER_SEMAPHORE                             0xFD0
#define XE2_REG_GGC                                         0x108040 // Graphics control
#define XE2_REG_GGC_GMS_MASK                                xe2_reg_genmask(15, 8)
#define XE2_REG_GGC_GGMS_MASK                               xe2_reg_genmask(7, 6)

#define XE2_REG_DSMBASE_LO                                  0x1080C0
#define XE2_REG_DSMBASE_HI                                  0x1080C4
#define XE2_REG_DSMBASE_64_MASK                             xe2_reg_genmask64(63, 20)

#define XE2_REG_XEHP_TILE_ADDRESS_RANGE(n)                  (0x4900 + (n) * 4)

#define XE2_REG_GSMBASE_LO                                  0x108100
#define XE2_REG_GSMBASE_HI                                  0x108104

#define XE2_REG_GUC_TLB_INV_DESC0                           0xCF7C // Write-only for OS
#define XE2_REG_GUC_TLB_INV_DESC1                           0xCF80 // Write-only for OS

#define XE2_REG_GU_CNTL_PROTECTED                           0x10100C // This being used from i915
#define XE2_REG_GU_CNTL_PROTECTED_PRESENT_MASK              xe2_reg_genmask(9, 9)

#define XE2_REG_VF_CAP                                      0x1901F8
#define XE2_REG_VF_CAP_MASK                                 xe2_reg_genmask(0, 0)

#define XE2_REG_SDEISR                                      0xC4000
#define XE2_REG_SDEIMR                                      0xC4004
#define XE2_REG_SDEIIR                                      0xC4008
#define XE2_REG_SDEIER                                      0xC400C

#define XE2_REG_PSR_IMR_A                                   0x60814
#define XE2_REG_PSR_IIR_A                                   0x60818

#define XE2_REG_DE_PORT_ISR                                 0x44440
#define XE2_REG_DE_PORT_IMR                                 0x44444
#define XE2_REG_DE_PORT_IIR                                 0x44448
#define XE2_REG_DE_PORT_IER                                 0x4444c

#define XE2_REG_GU_MISC_ISR                                 0x444f0
#define XE2_REG_GU_MISC_IMR                                 0x444f4
#define XE2_REG_GU_MISC_IIR                                 0x444f8
#define XE2_REG_GU_MISC_IER                                 0x444fc

#define XE2_REG_DE_PIPE_ISR(pipe)                           (0x44400 + (0x10 * (pipe)))
#define XE2_REG_DE_PIPE_IMR(pipe)                           (0x44404 + (0x10 * (pipe)))
#define XE2_REG_DE_PIPE_IIR(pipe)                           (0x44408 + (0x10 * (pipe)))
#define XE2_REG_DE_PIPE_IER(pipe)                           (0x4440C + (0x10 * (pipe)))

#define XE2_PIPE_A                                          0x0
#define XE2_PIPE_B                                          0x1
#define XE2_PIPE_C                                          0x2
#define XE2_PIPE_D                                          0x3

#define XE2_REG_DE_MISC_ISR                                 0x44460
#define XE2_REG_DE_MISC_IMR                                 0x44464
#define XE2_REG_DE_MISC_IIR                                 0x44468
#define XE2_REG_DE_MISC_IER                                 0x4446C

#define XE2_REG_DPA_AUX_CH_DATA(n)                          (0x64014 + 4 * (n))
#define XE2_REG_DPB_AUX_CH_DATA(n)                          (0x64114 + 4 * (n))
#define XE2_REG_DPX_AUX_CH_DATA_INDEX(reg)                  ((reg - 0x64014) / 4)

#define XE2_REG_DPA_AUX_CH_CTL                              0x64010
#define XE2_REG_DPB_AUX_CH_CTL                              0x64110
#define XE2_REG_DPX_AUX_CH_CTL_SEND_BUSY_MASK               xe2_reg_bit(31)
#define XE2_REG_DPX_AUX_CH_CTL_DONE_MASK                    xe2_reg_bit(30)
#define XE2_REG_DPX_AUX_CH_CTL_INTERRUPT_MASK               xe2_reg_bit(29)
#define XE2_REG_DPX_AUX_CH_CTL_TIME_OUT_ERROR_MASK          xe2_reg_bit(28)
#define XE2_REG_DPX_AUX_CH_CTL_TIME_OUT_MASK                xe2_reg_genmask(27, 26)
#define XE2_REG_DPX_AUX_CH_CTL_RECEIVE_ERROR_MASK           xe2_reg_bit(25)
#define XE2_REG_DPX_AUX_CH_CTL_MSG_SIZE_MASK                xe2_reg_genmask(24, 20)
#define XE2_REG_DPX_AUX_CH_CTL_PRECHARGE_2US_MASK           xe2_reg_genmask(19, 16)
#define XE2_REG_DPX_AUX_CH_CTL_AUX_AKSV_SELECT_MASK         xe2_reg_bit(15)
#define XE2_REG_DPX_AUX_CH_CTL_MANCHESTER_MASK              xe2_reg_bit(14)
#define XE2_REG_DPX_AUX_CH_CTL_PSR_DATA_AUX_SKL_MASK        xe2_reg_bit(14)
#define XE2_REG_DPX_AUX_CH_CTL_SYNC_TEST_MASK               xe2_reg_bit(13)
#define XE2_REG_DPX_AUX_CH_CTL_FS_DATA_AUX_SKL_MASK         xe2_reg_bit(13)
#define XE2_REG_DPX_AUX_CH_CTL_DEGLITCH_TEST__MASK          xe2_reg_bit(12)
#define XE2_REG_DPX_AUX_CH_CTL_GTC_DATA_AUX_REG_MASK        xe2_reg_bit(12)
#define XE2_REG_DPX_AUX_CH_CTL_PRECHARGE_TEST_MASK          xe2_reg_bit(11)
#define XE2_REG_DPX_AUX_CH_CTL_TBT_IO_MASK                  xe2_reg_bit(11)
#define XE2_REG_DPX_AUX_CH_CTL_BIT_CLOCK_2X_MASK            xe2_reg_genmask(10, 0)
#define XE2_REG_DPX_AUX_CH_CTL_FW_SYNC_PULSE_SKL_MASK       xe2_reg_genmask(9, 5)
#define XE2_REG_DPX_AUX_CH_CTL_SYNC_PUSLE_SKL_MASK          xe2_reg_genmask(4, 0)

#define XE2_REG_PP_STATUS                                   0x61200 // Panel power sequence
#define XE2_REG_PP_ON_MASK                                  xe2_reg_bit(31)
#define XE2_REG_PP_READY_MASK                               xe2_reg_bit(30)
#define XE2_REG_PP_SEQUENCE_MASK                            xe2_reg_genmask(29, 28)
#define XE2_REG_PP_CYCLE_DELAY_ACTIVE_MASK                  xe2_reg_bit(27)

#define XE2_REG_PP_CONTROL                                  0x61204
#define XE2_REG_PP_CONTROL_UNLOCK_MASK                      xe2_reg_genmask(31, 16)
#define XE2_REG_PP_CONTROL_POWER_CYCLE_DELAY_MASK           xe2_reg_genmask( 8,  4)
#define XE2_REG_PP_CONTROL_EPD_FORCE_VDD_MASK               xe2_reg_bit(3)
#define XE2_REG_PP_CONTROL_EPD_BLC_ENABLE_MASK              xe2_reg_bit(2)
#define XE2_REG_PP_CONTROL_POWER_RESET_MASK                 xe2_reg_bit(1)
#define XE2_REG_PP_CONTROL_POWER_ON_MASK                    xe2_reg_bit(0)

#define XE2_REG_PP_ON_DELAYS                                0x61208
#define XE2_REG_PP_ON_DELAYS_PORT_SELECT_MASK               xe2_reg_genmask(31, 30)
#define XE2_REG_PP_ON_DELAYS_POWER_ON_DELAY_MASK            xe2_reg_genmask(28, 16)
#define XE2_REG_PP_ON_DELAYS_LIGHT_ON_DELAY_MASK            xe2_reg_genmask(12,  0)

#define XE2_REG_PP_OFF_DELAYS                               0x6120C
#define XE2_REG_PP_OFF_DELAYS_POWER_DOWN_DELAY_MASK         xe2_reg_genmask(28, 16)
#define XE2_REG_PP_OFF_DELAYS_LIGHT_OFF_DELAY_MASK          xe2_reg_genmask(12,  0)

#define XE2_REG_PP_DIVISOR                                  0x61210
#define XE2_REG_PP_DIVISOR_REF_DIVIDER_MASK                 xe2_reg_genmask(31,  8)
#define XE2_REG_PP_DIVISOR_POWER_CYCLE_DELAY_MASK           xe2_reg_genmask( 4,  0)

#define XE2_REG_PCH_PP_STATUS                               0xC7200
#define XE2_REG_PCH_PP_CONTROL                              0xC7204
#define XE2_REG_PCH_PP_ON_DELAYS                            0xC7208
#define XE2_REG_PCH_PP_OFF_DELAYS                           0xC720C
#define XE2_REG_PCH_PP_DIVISOR                              0xC7210

#define XE2_REG_RP_CONTROL                                  0xA024
#define XE2_REG_RP_CONTROL_RPSWCTL_MASK                     xe2_reg_genmask(10, 9)

#define XE2_REG_RC_CONTROL                                  0xA090
#define XE2_REG_RC_CONTROL_CTL_HW_ENABLE_MASK               xe2_reg_bit(31)
#define XE2_REG_RC_CONTROL_CTL_TO_MODE_MASK                 xe2_reg_bit(28)
#define XE2_REG_RC_CONTROL_CTL_RC6_ENABLE_MASK              xe2_reg_bit(18)

#define XE2_REG_HSW_POWER_WELL_CTL1                         0x45400
#define XE2_REG_HSW_POWER_WELL_CTL2                         0x45404
#define XE2_REG_HSW_POWER_WELL_CTL3                         0x45408
#define XE2_REG_HSW_POWER_WELL_CTL4                         0x4540C

#define XE2_REG_BXT_DE_PLL_ENABLE                           0x46070
#define XE2_REG_BXT_DE_PLL_ENABLE_MASK                      xe2_reg_bit(31)
#define XE2_REG_BXT_DE_PLL_ENABLE_LOCK_MASK                 xe2_reg_bit(30)
#define XE2_REG_BXT_DE_PLL_ENABLE_FREQ_REQ_MASK             xe2_reg_bit(23)
#define XE2_REG_BXT_DE_PLL_ENABLE_FREQ_REQ_ACK_MASK         xe2_reg_bit(22)

#define XE2_REG_SKL_FUSE_STATUS                             0x42000
#define XE2_REG_SKL_FUSE_STATUS_DOWNLOAD_MASK               xe2_reg_bit(31)
// Power gates:
//   SKL_PG0 = 0
//   SKL_PG1 = 1
//   SKL_PG2 = 2
//   ICL_PG3 = 3
//   ICL_PG4 = 4
#define XE2_REG_SKL_FUSE_STATUS_DST_MASK(pg)                (1 << (27 - (pg)))

#define XE2_REG_DBUF_CTL_S0                                 0x45008
#define XE2_REG_DBUF_CTL_S1                                 0x44FE8
#define XE2_REG_DBUF_CTL_S2                                 0x44300
#define XE2_REG_DBUF_CTL_S3                                 0x44304
#define XE2_REG_DBUF_CTL_SX_POWER_REQUEST_MASK              xe2_reg_bit(31)
#define XE2_REG_DBUF_CTL_SX_POWER_STATE_MASK                xe2_reg_bit(30)
#define XE2_REG_DBUF_CTL_SX_TRACKER_STATE_SERVICE_MASK      xe2_reg_genmask(23, 19)
#define XE2_REG_DBUF_CTL_SX_MIN_TRACKER_STATE_SERVICE_MASK  xe2_reg_genmask(18, 16)

#define XE2_REG_CDCLK_CTL                                   0x46000 // Core display clock
#define XE2_REG_CDCLK_CTL_FREQ_SEL_MASK                     xe2_reg_genmask(27, 26)
#define XE2_REG_CDCLK_CTL_SOURCE_SEL_MASK                   xe2_reg_bit(25)
#define XE2_REG_CDCLK_CTL_CD2X_DIV_SEL_MASK                 xe2_reg_genmask(23, 22)
#define XE2_REG_CDCLK_CTL_CD2X_PIPE_MASK                    xe2_reg_genmask(21, 20)

#define XE2_REG_SKL_DSSM                                    0x51004 // Reference CDCLK
#define XE2_REG_SKL_DSSM_PLL_REFCLK_MASK                    (7U << 29)
#define XE2_REG_SKL_DSSM_PLL_REFCLK_24MHZ                   (0U << 29)
#define XE2_REG_SKL_DSSM_PLL_REFCLK_19_2MHZ                 (1U << 29)
#define XE2_REG_SKL_DSSM_PLL_REFCLK_38_4MHZ                 (2U << 29)

#define XE2_REG_DC_STATE_EN                                 0x45504
#define XE2_REG_DC_STATE_EN_DC3C0_MASK                      xe2_reg_bit(30)
#define XE2_REG_DC_STATE_DC3CO_STATUS_MASK                  xe2_reg_bit(29)
#define XE2_REG_DC_STATE_EN_DC3C0                           (1 << 30)
#define XE2_REG_DC_STATE_EN_UPTO_DC5                        (1 <<  0)
#define XE2_REG_DC_STATE_EN_UPTO_DC6                        (2 <<  0)
#define XE2_REG_DC_STATE_EN_DC9                             (1 <<  3)

#define XE2_REG_ILK_DPFC_CONTROL_1                          0x43208
#define XE2_REG_ILK_DPFC_CONTROL_2                          0x43248
#define XE2_REG_ILK_DPFC_CONTROL_X_EN_MASK                  xe2_reg_bit(31)

#define XE2_REG_GUC_WOPCM_OFFSET                            0xC340
#define XE2_REG_GUC_WOPCM_OFFSET_MASK                       xe2_reg_genmask(31, 14)
#define XE2_REG_HUC_LOADING_AGENT_GUC_MASK                  xe2_reg_bit(1)
#define XE2_REG_GUC_WOPCM_OFFSET_VALID                      xe2_reg_bit(0)

// WOPCM (Write once protected content memory)
// https://docs.kernel.org/gpu/xe/xe_firmware.html
#define XE2_REG_GUC_WOPCM_SIZE                              0xC050
#define XE2_REG_GUC_WOPCM_SIZE_MASK                         xe2_reg_genmask(31, 12)
#define XE2_REG_GUC_WOPCM_SIZE_LOCKED_MASK                  xe2_reg_bit(0)

#define XE2_REG_GUC_PVC_TLB_INV_DESC0                       0xCF7C
#define XE2_REG_GUC_PVC_TLB_INV_DESC1                       0xCF80

#define XE2_REG_GUC_STATUS                                  0xC000 // Probably RO
#define XE2_REG_GUC_STATUS_MASK                             xe2_reg_genmask(31, 30)
#define XE2_REG_GUC_STATUS_MIA_MASK                         xe2_reg_genmask(18, 16)
#define XE2_REG_GUC_STATUS_UKERNEL_MASK                     xe2_reg_genmask(15, 8)
#define XE2_REG_GUC_STATUS_BOOTROM_MASK                     xe2_reg_genmask(7, 1)
#define XE2_REG_GUC_STATUS_MIA_IN_RESET_MASK                xe2_reg_bit(0)

#define XE2_REG_GUC_FW_SW_1                                 0x190240
#define XE2_REG_GUC_FW_SW_2                                 0x190244
#define XE2_REG_GUC_FW_SW_3                                 0x190248
#define XE2_REG_GUC_FW_SW_4                                 0x19024C
#define XE2_REG_GUC_FW_SW_X_MSG_0_ORIGIN_MASK               xe2_reg_bit(31)
#define XE2_REG_GUC_FW_SW_X_MSG_0_TYPE_MASK                 xe2_reg_genmask(30, 28)
#define XE2_REG_GUC_FW_SW_X_MSG_0_DATA_MASK                 xe2_reg_genmask(27, 0)

#define XE2_REG_GUC_HOST_INTERRUPT                          0x1901F0

#define XE2_REG_GUC_PMTIMESTAMP_LO                          0xC3E8
#define XE2_REG_GUC_PMTIMESTAMP_HI                          0xC3EC

#define XE2_REG_GUC_DMA_ADDR_0_LO                           0xC300
#define XE2_REG_GUC_DMA_ADDR_0_HI                           0xC304
#define XE2_REG_GUC_DMA_ADDR_1_LO                           0xC308
#define XE2_REG_GUC_DMA_ADDR_1_HI                           0xC30C
#define XE2_REG_GUC_DMA_COPY_SIZE                           0xC310
#define XE2_REG_GUC_DMA_CTRL                                0xC314
#define XE2_REG_GUC_DMA_CTRL_HUC_UKERNEL                    xe2_reg_bit(9)
#define XE2_REG_GUC_DMA_CTRL_UOS_MOVE                       xe2_reg_bit(4)
#define XE2_REG_GUC_DMA_CTRL_START_DMA                      xe2_reg_bit(0)

#define XE2_REG_GUC_SOFT_SCRATCH(n)                         (0xC180 + (n) * 4)
#define XE2_REG_GUC_SOFT_SCRATCH_INDEX(reg)                 ((reg - 0xC180) / 4)
#define XE2_GUC_SOFT_SCRATCH_COUNT                          16

#define XE2_REG_STOLEN_RESERVED_LO                          0x1082C0 // Das war schön gestohlen mal...
#define XE2_REG_STOLEN_RESERVED_HI                          0x1082C4
#define XE2_REG_STOLEN_RESERVED_WOPCM_SIZE_MASK             xe2_reg_genmask(9, 7)

#define XE2_DMC_FW_MAIN                                     0
#define XE2_DMC_FW_PIPE_A                                   1
#define XE2_DMC_FW_PIPE_B                                   2
#define XE2_DMC_FW_PIPE_C                                   3
#define XE2_DMC_FW_PIPE_D                                   4
#define XE2_DMC_FW_PIPE_TOTAL                               5

#define XE2_DMC_FW_MAIN_OFFSET                              0x80000
#define XE2_DMC_FW_PIPE_A_OFFSET                            0x90000
#define XE2_DMC_FW_PIPE_B_OFFSET                            0x98000
#define XE2_DMC_FW_PIPE_C_OFFSET                            0x52000
#define XE2_DMC_FW_PIPE_D_OFFSET                            0x59000

#define XE2_REG_DMC_SSP_BASE                                0x8F074

#define XE2_REG_XELP_GT_GEOMETRY_DSS_ENABLE                 0x913C
#define XE2_REG_XELP_XEHP_GT_COMPUTE_DSS_ENABLE             0x9144
#define XE2_REG_XELP_EU_ENABLE                              0x9134
#define XE2_REG_MIRROR_FUSE3                                0x9118
#define XE2_REG_MIRROR_FUSE3_NODE_ENABLE_MASK               xe2_reg_genmask(31, 16)
#define XE2_REG_MIRROR_FUSE3_XEHPC_GT_L3_MODE_MASK          xe2_reg_genmask( 7,  4)
#define XE2_REG_MIRROR_FUSE3_MEML3_EN_MASK                  xe2_reg_genmask( 3,  0)

#define XE2_REG_PLANE_CTL_1_A                               0x70180 // We assume post-icl graphics
#define XE2_REG_PLANE_CTL_2_A                               0x70280
#define XE2_REG_PLANE_CTL_1_B                               0x71180
#define XE2_REG_PLANE_CTL_2_B                               0x71280
#define XE2_REG_PLANE_CTL_X_ICL_FORMAT_MASK                 xe2_reg_genmask(27, 23)
#define XE2_REG_PLANE_CTL_X_KEY_ENABLE_MASK                 xe2_reg_genmask(22, 21)
#define XE2_REG_PLANE_CTL_X_ORDER_RGBX_MASK                 xe2_reg_bit(20)
#define XE2_REG_PLANE_CTL_X_YUV420_Y_PLANE_MASK             xe2_reg_bit(19)
#define XE2_REG_PLANE_CTL_X_YUV_TO_RGB_CSC_FORMAT_BT709_MASK \
                                                            xe2_reg_bit(18)
#define XE2_REG_PLANE_CTL_X_YUV422_ORDER_MASK               xe2_reg_genmask(17, 16)
#define XE2_REG_PLANE_CTL_X_RENDER_DECOMPRESSION_ENABLE_MASK \
                                                            xe2_reg_bit(15)
#define XE2_REG_PLANE_CTL_X_TRICKLE_FEED_DISABLE_MASK       xe2_reg_bit(14)
#define XE2_REG_PLANE_CTL_X_CLEAR_COLOR_DISABLE_MASK        xe2_reg_bit(13)
#define XE2_REG_PLANE_CTL_X_TILED_MASK                      xe2_reg_genmask(12, 10)
#define XE2_REG_PLANE_CTL_X_ASYNC_FLIP_MASK                 xe2_reg_bit(9)
#define XE2_REG_PLANE_CTL_X_FLIP_HORIZONTAL_MASK            xe2_reg_bit(8)
#define XE2_REG_PLANE_CTL_X_MEDIA_DECOMPRESSION_ENABLE_MASK xe2_reg_bit(4)
#define XE2_REG_PLANE_CTL_X_ROTATE_MASK                     xe2_reg_genmask(1, 0)

#define XE2_GUC_ACTION_DEFAULT                              0x0
#define XE2_GUC_ACTION_REQUEST_PREEMPTION                   0x2
#define XE2_GUC_ACTION_REQUEST_ENGINE_RESET                 0x3
#define XE2_GUC_ACTION_ALLOCATE_DOORBELL                    0x10
#define XE2_GUC_ACTION_DEALLOCATE_DOORBELL                  0x20
#define XE2_GUC_ACTION_LOG_BUFFER_FILE_FLUSH_COMPLETE       0x30
#define XE2_GUC_ACTION_UK_LOG_ENABLE_LOGGING                0x40
#define XE2_GUC_ACTION_FORCE_LOG_BUFFER_FLUSH               0x302
#define XE2_GUC_ACTION_ENTER_S_STATE                        0x501
#define XE2_GUC_ACTION_EXIT_S_STATE                         0x502
#define XE2_GUC_ACTION_GLOBAL_SCHED_POLICY_CHANGE           0x506
#define XE2_GUC_ACTION_HOST2GUC_SELF_CFG                    0x508
#define XE2_GUC_ACTION_UPDATE_SCHEDULING_POLICIES_KLV       0x509
#define XE2_GUC_ACTION_SCHED_CONTEXT                        0x1000
#define XE2_GUC_ACTION_SCHED_CONTEXT_MODE_SET               0x1001
#define XE2_GUC_ACTION_SCHED_CONTEXT_MODE_DONE              0x1002
#define XE2_GUC_ACTION_SCHED_ENGINE_MODE_SET                0x1003
#define XE2_GUC_ACTION_SCHED_ENGINE_MODE_DONE               0x1004
#define XE2_GUC_ACTION_SET_CONTEXT_PRIORITY                 0x1005
#define XE2_GUC_ACTION_SET_CONTEXT_EXECUTION_QUANTUM        0x1006
#define XE2_GUC_ACTION_SET_CONTEXT_PREEMPTION_TIMEOUT       0x1007
#define XE2_GUC_ACTION_CONTEXT_RESET_NOTIFICATION           0x1008
#define XE2_GUC_ACTION_ENGINE_FAILURE_NOTIFICATION          0x1009
#define XE2_GUC_ACTION_HOST2GUC_UPDATE_CONTEXT_POLICIES     0x100B
#define XE2_GUC_ACTION_AUTHENTICATE_HUC                     0x4000
#define XE2_GUC_ACTION_GET_HWCONFIG                         0x4100
#define XE2_GUC_ACTION_REGISTER_CONTEXT                     0x4502
#define XE2_GUC_ACTION_DEREGISTER_CONTEXT                   0x4503
#define XE2_GUC_ACTION_REGISTER_COMMAND_TRANSPORT_BUFFER    0x4505
#define XE2_GUC_ACTION_DEREGISTER_COMMAND_TRANSPORT_BUFFER  0x4506
#define XE2_GUC_ACTION_REGISTER_G2G                         0x4507
#define XE2_GUC_ACTION_DEREGISTER_G2G                       0x4508
#define XE2_GUC_ACTION_HOST2GUC_CONTROL_CTB                 0x4509
#define XE2_GUC_ACTION_DEREGISTER_CONTEXT_DONE              0x4600
#define XE2_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC           0x4601
#define XE2_GUC_ACTION_CLIENT_SOFT_RESET                    0x5507
#define XE2_GUC_ACTION_SET_ENG_UTIL_BUFF                    0x550A
#define XE2_GUC_ACTION_SET_DEVICE_ENGINE_ACTIVITY_BUFFER    0x550C
#define XE2_GUC_ACTION_SET_FUNCTION_ENGINE_ACTIVITY_BUFFER  0x550D
#define XE2_GUC_ACTION_OPT_IN_FEATURE_KLV                   0x550E
#define XE2_GUC_ACTION_NOTIFY_MEMORY_CAT_ERROR              0x6000
#define XE2_GUC_ACTION_REPORT_PAGE_FAULT_REQ_DESC           0x6002
#define XE2_GUC_ACTION_PAGE_FAULT_RES_DESC                  0x6003
#define XE2_GUC_ACTION_ACCESS_COUNTER_NOTIFY                0x6004
#define XE2_GUC_ACTION_TLB_INVALIDATION                     0x7000
#define XE2_GUC_ACTION_TLB_INVALIDATION_DONE                0x7001
#define XE2_GUC_ACTION_TLB_INVALIDATION_ALL                 0x7002
#define XE2_GUC_ACTION_STATE_CAPTURE_NOTIFICATION           0x8002
#define XE2_GUC_ACTION_NOTIFY_FLUSH_LOG_BUFFER_TO_FILE      0x8003
#define XE2_GUC_ACTION_NOTIFY_CRASH_DUMP_POSTED             0x8004
#define XE2_GUC_ACTION_NOTIFY_EXCEPTION                     0x8005
#define XE2_GUC_ACTION_TEST_G2G_SEND                        0xF001
#define XE2_GUC_ACTION_TEST_G2G_RECV                        0xF002

#define GUC_KLV_SELF_CFG_MEMIRQ_STATUS_ADDR_KEY             0x900
#define GUC_KLV_SELF_CFG_MEMIRQ_STATUS_ADDR_LEN             2
#define GUC_KLV_SELF_CFG_MEMIRQ_SOURCE_ADDR_KEY             0x901
#define GUC_KLV_SELF_CFG_MEMIRQ_SOURCE_ADDR_LEN             2
#define GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY                   0x902
#define GUC_KLV_SELF_CFG_H2G_CTB_ADDR_LEN                   2
#define GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY        0x903
#define GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_LEN        2
#define GUC_KLV_SELF_CFG_H2G_CTB_SIZE_KEY                   0x904
#define GUC_KLV_SELF_CFG_H2G_CTB_SIZE_LEN                   1
#define GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY                   0x905
#define GUC_KLV_SELF_CFG_G2H_CTB_ADDR_LEN                   2
#define GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY        0x906
#define GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_LEN        2
#define GUC_KLV_SELF_CFG_G2H_CTB_SIZE_KEY                   0x907
#define GUC_KLV_SELF_CFG_G2H_CTB_SIZE_LEN                   1

// GuC Command Transport Buffer (CTB) message framing. Each ring message is one
// CTB header dword followed by NUM_DWORDS payload dwords. The buffer descriptor
// holds head (consumer) and tail (producer) as dword indices into a circular
// command buffer.
#define XE2_GUC_CTB_HDR_LEN                                 1
#define XE2_GUC_CTB_MSG_0_FENCE                             xe2_reg_genmask(31, 16)
#define XE2_GUC_CTB_MSG_0_FORMAT                            xe2_reg_genmask(15, 12)
#define XE2_GUC_CTB_FORMAT_HXG                              0
#define XE2_GUC_CTB_MSG_0_NUM_DWORDS                        xe2_reg_genmask(7, 0)

// Host <-> GuC (HXG) message header, carried as the first payload dword.
#define XE2_GUC_HXG_MSG_0_ORIGIN                            xe2_reg_bit(31)
#define XE2_GUC_HXG_ORIGIN_GUC                              1
#define XE2_GUC_HXG_MSG_0_TYPE                              xe2_reg_genmask(30, 28)
#define XE2_GUC_HXG_TYPE_REQUEST                            0
#define XE2_GUC_HXG_TYPE_EVENT                              1
#define XE2_GUC_HXG_TYPE_RESPONSE_SUCCESS                  7
#define XE2_GUC_HXG_MSG_0_ACTION                           xe2_reg_genmask(15, 0)
#define XE2_GUC_HXG_RESPONSE_MSG_0_DATA0                   xe2_reg_genmask(27, 0)

// Logical Ring Context (LRC) image layout. The register state follows the
// per-process HW status page (PPHWSP), one page in. Each entry below is a dword
// index into that register state; the value lives at (LRC base + 0x1000 + i*4).
#define XE2_LRC_REGS_OFFSET                               0x1000
#define XE2_CTX_RING_HEAD                                 5
#define XE2_CTX_RING_TAIL                                 7
#define XE2_CTX_RING_START                                9
#define XE2_CTX_RING_CTL                                  11
#define XE2_CTX_INT_STATUS_REPORT_PTR                     87
#define XE2_CTX_INT_SRC_REPORT_PTR                        89

// Ring command stream decode. Type lives in bits 31:29 (MI = 0, GFXPIPE = 3);
// the MI opcode is bits 28:23. The completion postamble a job appends to its
// ring stores the seqno (MI_STORE_DATA_IMM / MI_FLUSH_DW / PIPE_CONTROL with a
// GGTT post-sync write) and raises MI_USER_INTERRUPT.
#define XE2_INSTR_TYPE(h)                                 ((h) >> 29)
#define XE2_INSTR_TYPE_MI                                 0
#define XE2_INSTR_TYPE_GFXPIPE                            3
#define XE2_MI_OPCODE(h)                                  (((h) >> 23) & 0x3f)
#define XE2_MI_OP_NOOP                                    0x00
#define XE2_MI_OP_USER_INTERRUPT                          0x02
#define XE2_MI_OP_ARB_CHECK                               0x05
#define XE2_MI_OP_ARB_ON_OFF                              0x08
#define XE2_MI_OP_BATCH_BUFFER_END                        0x0a
#define XE2_MI_OP_STORE_DATA_IMM                          0x20
#define XE2_MI_OP_FLUSH_DW                                0x26
#define XE2_MI_SDI_GGTT                                   (1u << 22)
#define XE2_MI_FLUSH_DW_OP_STOREDW                        (1u << 14)
#define XE2_MI_FLUSH_DW_USE_GTT                           (1u << 2)
#define XE2_PIPE_CONTROL_SIG                              0x7a // (h >> 24)
#define XE2_PIPE_CONTROL_QW_WRITE                         (1u << 14)
#define XE2_PIPE_CONTROL_GLOBAL_GTT                       (1u << 24)

// Memory-based interrupts (memirq). The engine's source-report and
// status-report GGTT pointers are baked into its LRC register state (the driver
// never sends them over self-cfg). The render engine sits at irq_offset 0, and
// its user-interrupt is byte 0 of the status vector; each cause is a byte 0xFF.
#define XE2_MEMIRQ_RENDER_SRC_BYTE                        0
#define XE2_MEMIRQ_RENDER_STATUS_BYTE                     0
#define XE2_MEMIRQ_BYTE_SET                               0xFF
#define XE2_LRC_SEQNO_OFFSET                              0

// memirq page geometry. The source-report page sits 0x400 into the shared BO;
// each status vector is 16 bytes. The GuC's own interrupt sits at bit 25
// (INTR_GUC): source byte at +0x400+25, status vector at +25*16, and its
// GuC-to-host cause is byte 15 (GUC_INTR_GUC2HOST) of that vector.
#define XE2_MEMIRQ_SOURCE_PAGE_OFFSET                     0x400
#define XE2_MEMIRQ_VECTOR_STRIDE                          16
#define XE2_MEMIRQ_INTR_GUC                               25
#define XE2_MEMIRQ_GUC2HOST_BYTE                          15


#define XE2_HW_ENGINE_RENDER_RING_BASE                      0x02000
#define XE2_HW_ENGINE_BSD_RING_BASE                         0x1C0000
#define XE2_HW_ENGINE_BSD2_RING_BASE                        0x1C4000
#define XE2_HW_ENGINE_BSD3_RING_BASE                        0x1D0000
#define XE2_HW_ENGINE_BSD4_RING_BASE                        0x1D4000
#define XE2_HW_ENGINE_XEHP_BSD5_RING_BASE                   0x1E0000
#define XE2_HW_ENGINE_XEHP_BSD6_RING_BASE                   0x1E4000
#define XE2_HW_ENGINE_XEHP_BSD7_RING_BASE                   0x1F0000
#define XE2_HW_ENGINE_XEHP_BSD8_RING_BASE                   0x1F4000
#define XE2_HW_ENGINE_VEBOX_RING_BASE                       0x1C8000
#define XE2_HW_ENGINE_VEBOX2_RING_BASE                      0x1D8000
#define XE2_HW_ENGINE_XEHP_VEBOX3_RING_BASE                 0x1E8000
#define XE2_HW_ENGINE_XEHP_VEBOX4_RING_BASE                 0x1F8000
#define XE2_HW_ENGINE_COMPUTE0_RING_BASE                    0x1A000
#define XE2_HW_ENGINE_COMPUTE1_RING_BASE                    0x1C000
#define XE2_HW_ENGINE_COMPUTE2_RING_BASE                    0x1E000
#define XE2_HW_ENGINE_COMPUTE3_RING_BASE                    0x26000
#define XE2_HW_ENGINE_BLT_RING_BASE                         0x22000
#define XE2_HW_ENGINE_XEHPC_BCS1_RING_BASE                  0x3E0000
#define XE2_HW_ENGINE_XEHPC_BCS2_RING_BASE                  0x3E2000
#define XE2_HW_ENGINE_XEHPC_BCS3_RING_BASE                  0x3E4000
#define XE2_HW_ENGINE_XEHPC_BCS4_RING_BASE                  0x3E6000
#define XE2_HW_ENGINE_XEHPC_BCS5_RING_BASE                  0x3E8000
#define XE2_HW_ENGINE_XEHPC_BCS6_RING_BASE                  0x3EA000
#define XE2_HW_ENGINE_XEHPC_BCS7_RING_BASE                  0x3EC000
#define XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE                  0x3EE000
#define XE2_HW_ENGINE_GSCCS_RING_BASE                       0x11A000

#define XE2_REG_HW_ENGINE_CLASS(base)                       (base + 0x8C)
#define XE2_REG_HW_ENGINE_CLASS_INSTANCE_ID_MASK            xe2_reg_genmask(9, 4)
#define XE2_REG_HW_ENGINE_CLASS_ID_MASK                     xe2_reg_genmask(2, 0)

#define XE2_REG_HW_ENGINE_RING_IDLEDLY(base)                (base + 0x23C)
#define XE2_REG_HW_ENGINE_RING_IDLEDLY_INHIBIT_SWITCH_UNTIL_PREEMPTED_MASK \
                                                            xe2_reg_bit(31)
#define XE2_REG_HW_ENGINE_RING_IDLEDLY_IDLE_DELAY_MASK      xe2_reg_genmask(20, 0)

#define XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(base)          (base + 0x54)
#define XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT_IDLE_WAIT_TIME_MASK \
                                                            xe2_reg_genmask(19, 0)

#define XE2_REG_HW_ENGINE_RING_MI_MODE(base)                (base + 0x9C)

#define XE2_REG_HW_ENGINE_RING_TAIL(base)                   (base + 0x30)
#define XE2_REG_HW_ENGINE_RING_HEAD(base)                   (base + 0x34)
#define XE2_REG_HW_ENGINE_RING_START(base)                  (base + 0x38)
#define XE2_REG_HW_ENGINE_RING_CTL(base)                    (base + 0x3C)

#define XE2_VRAM_SIZE                                       0x10000000 // 256 MiB

// https://lists.freedesktop.org/archives/intel-xe/2023-June/005371.html
#define XE2_GGTT_PTE_VALID                                  (1ULL << 0)
#define XE2_GGTT_PAGES                                      0x100000
#define XE2_GGTT_PTE_ADDR_MASK                              0x0000FFFFFFFFF000ULL
#define XE2_GGTT_MMIO_BASE                                  0x800000 // 8 MiB
#define XE2_GGTT_MMIO_SIZE                                  0x800000 // 8 MiB

// DPCD (DispalyPort configuration data) is GPU-independent standard.
// May be applied elsewhere.
#define DPCD_REG_REV                                        0x00
#define DPCD_REG_RECEIVER_ALPM_CAP                          0x2E
#define DPCD_REG_DSC_SUPPORT                                0x60
#define DPCD_REG_PSR_SUPPORT                                0x70
#define DPCD_REG_PANEL_REPLAY_CAP_SUPPORT                   0xB0
#define DPCD_REG_SOURCE_OUI                                 0x300

// EDID address and size is not part of DPCD.
#define DPCD_INTEL_EDID_ADDR                                0x50
#define DPCD_INTEL_EDID_SIZE                                128

// https://docs.amd.com/r/en-US/pg199-displayport-tx-subsystem/I2C-Over-AUX-Transactions
//
// Pay attention: Driver natively emulates I2C transactions. Thus, pseudo-I2C
// commands goes through PCI.
#define DPCD_REQ_I2C_WRITE                                  0x0
#define DPCD_REQ_I2C_READ                                   0x1
#define DPCD_REQ_I2C_WRITE_STATUS_UPDATE                    0x2
#define DPCD_REQ_I2C_WRITE_MOT                              0x4 // Middle of transaction
#define DPCD_REQ_I2C_READ_MOT                               0x5 // Middle of transaction
#define DPCD_REQ_NATIVE_WRITE                               0x8
#define DPCD_REQ_NATIVE_READ                                0x9

// Auxiliary channel entry.
typedef struct {
    uint32_t data[5];       // Auxiliary channel data (5 dwords).
    uint32_t message_size;  // Size of AUX message. Encoded in incoming data[0].
    uint32_t ctl;           // DP(X)_AUX_CH_CTL register.
    uint32_t edid_written;  // Internal variable.
} xe2_aux_t;

#define XE2_MEM_SMEM        0 // System memory (accessed via DMA)
#define XE2_MEM_LMEM        1 // Local memory (accessed via VRAM)

typedef struct {
    uint64_t addr;
    uint8_t  type;
} xe2_dma_addr_t;

typedef struct {
    pci_func_t *pci_func;
    spinlock_t  lock;
    uint32_t    forcewake_gsc;
    uint32_t    forcewake_gt_mtl;
    uint32_t    forcewake_renderer;
    uint32_t    gt_gdrst;
    uint32_t    pll_enable;
    uint32_t    dbuf_ctl[4];
    uint32_t    pp_control;
    uint32_t    pp_status;
    uint32_t    pp_on_delays;
    uint32_t    pp_off_delays;
    uint32_t    dc_state;

    uint32_t    steer_semaphore;
    uint32_t    wopcm_size;
    uint32_t    wopcm_offset;
    uint32_t    wopcm_locked;

    xe2_aux_t   aux[1]; // We assume one display with one AUX channel.

    uint32_t    spi_address;
    uint32_t    spi_trigger;

    rvvm_addr_t dma_0;
    rvvm_addr_t dma_1;
    uint32_t    dma_copy_size;

    xe2_dma_addr_t hwlrca_addr;
    xe2_dma_addr_t pphwsp_addr;

    // Monotonic render-engine completion seqno, published at the LRC status page.
    uint32_t    ctx_seqno;

    struct {
        uint32_t       actions_h2g[4];
        uint32_t       actions_g2h[4];

        xe2_dma_addr_t memirq_status_addr;
        xe2_dma_addr_t memirq_source_addr;
        uint32_t       memirq_base_ggtt;
        xe2_dma_addr_t ctb_h2g_addr;
        size_t         ctb_h2g_size;
        xe2_dma_addr_t ctb_g2h_addr;
        size_t         ctb_g2h_size;
        xe2_dma_addr_t ctb_h2g_descriptor_addr;
        xe2_dma_addr_t ctb_g2h_descriptor_addr;
    } guc;

    // Page table entries.
    uint64_t    *ggtt_pte;
    // Lower PTE addresses. Comes from splitted
    // 64-bit MMIO request into two 32-bit ones.
    uint32_t    *ggtt_lo_addrs;
    // Validity map.
    bool        *ggtt_pte_valid;

    uint8_t     *vram;

    struct {
        // These sizes come from firmware blob.
        uint8_t main  [0x470C];
        uint8_t pipe_a[0x2864];
        uint8_t pipe_b[0x2D5C];
        uint8_t pipe_c[0x07D8];
        uint8_t pipe_d[0x07D8];

        uint32_t main_loaded;
        uint32_t pipe_a_loaded;
        uint32_t pipe_b_loaded;
        uint32_t pipe_c_loaded;
        uint32_t pipe_d_loaded;

        uint32_t dmc_base;
    } firmware;
} xe2_dev_t;

// Encoded configuration:
// | Block 0, Base EDID:
// |   EDID Structure Version & Revision: 1.4
// |   Vendor & Product Identification:
// |     Manufacturer: BOE
// |     Model: 1955
// |     Made in: week 1 of 2017
// |   Basic Display Parameters & Features:
// |     Digital display
// |     Bits per primary color channel: 6
// |     DisplayPort interface
// |     Maximum image size: 34 cm x 19 cm
// |     Gamma: 2.20
// |     Supported color formats: RGB 4:4:4
// |     First detailed timing includes the native pixel format and preferred refresh rate
// |   Color Characteristics:
// |     Red  : 0.5917, 0.3466
// |     Green: 0.3281, 0.5703
// |     Blue : 0.1503, 0.1142
// |     White: 0.3125, 0.3281
// |   Established Timings I & II: none
// |   Standard Timings: none
// |   Detailed Timing Descriptors:
// |     DTD 1:  1920x1080   60.000509 Hz  16:9     67.201 kHz    141.390000 MHz (344 mm x 193 mm)
// |                  Hfront   48 Hsync  32 Hback  104 Hpol P
// |                  Vfront    3 Vsync   6 Vback   31 Vpol N
// |     Empty Descriptor
// |     Alphanumeric Data String: 'BOE CQ'
// |     Alphanumeric Data String: 'NT156FHM-N41'
// | Checksum: 0x27
static const uint8_t xe2_edid[] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x09, 0xE5, 0xA3, 0x07, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x1B, 0x01, 0x04, 0x95, 0x22, 0x13, 0x78, 0x02, 0xB0, 0x90, 0x97, 0x58, 0x54, 0x92, 0x26,
    0x1D, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x3B, 0x37, 0x80, 0xB8, 0x70, 0x38, 0x28, 0x40, 0x30, 0x20,
    0x36, 0x00, 0x58, 0xC1, 0x10, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x42,
    0x4F, 0x45, 0x20, 0x43, 0x51, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFE,
    0x00, 0x4E, 0x54, 0x31, 0x35, 0x36, 0x46, 0x48, 0x4D, 0x2D, 0x4E, 0x34, 0x31, 0x0A, 0x00, 0x27,
};

static void xe2_remove(rvvm_mmio_dev_t *dev)
{
    xe2_dev_t *xe2 = dev->data;
    vma_free(xe2->ggtt_pte_valid, XE2_GGTT_PAGES * sizeof(bool));
    vma_free(xe2->ggtt_lo_addrs, XE2_GGTT_PAGES * sizeof(uint32_t));
    vma_free(xe2->ggtt_pte, XE2_GGTT_PAGES * sizeof(uint64_t));
    vma_free(xe2->vram, XE2_VRAM_SIZE);
}

static rvvm_mmio_type_t xe2_type = {
    .name = "xe2",
    .remove = xe2_remove,
};

static inline void xe2_ggtt_write_pte(xe2_dev_t *xe2, uint64_t index, uint64_t pte)
{
    xe2->ggtt_pte[index] = pte;
}

static inline void xe2_ggtt_mmio_write(xe2_dev_t *xe2, uint32_t offset, uint32_t value)
{
    uint64_t idx = (offset - XE2_GGTT_MMIO_BASE) / 8;
    bool     hi  =  offset & 4;

    if (!hi) {
        xe2->ggtt_lo_addrs[idx] = value;
        xe2->ggtt_pte_valid[idx] = true;
        return;
    }

    if (xe2->ggtt_pte_valid[idx]) {
        uint64_t pte = ((uint64_t) value << 32) | (uint64_t) xe2->ggtt_lo_addrs[idx];
        // rvvm_info("Write PTE[0x%lx]: 0x%lx", idx, pte);
        xe2_ggtt_write_pte(xe2, idx, pte);
    }
}

static inline xe2_dma_addr_t xe2_ggtt_translate(xe2_dev_t *xe2, uint64_t ggtt)
{
    uint64_t idx = ggtt >> 12;
    uint64_t off = ggtt & 0xfff;
    uint64_t pte = xe2->ggtt_pte[idx];

    rvvm_info("PTE: ggtt addr:       0x%lx", ggtt);
    rvvm_info("PTE: ggtt index:      0x%lx", idx);
    rvvm_info("PTE: ggtt off:        0x%lx", off);
    rvvm_info("PTE: raw pte:         0x%lx", pte);
    rvvm_info("PTE:   result:        0x%llx", (pte & 0x0000FFFFFFFFF000ULL) + off);
    rvvm_info("PTE: flag (NULL)?     %lu", pte & (1 << 9));
    rvvm_info("PTE: flag (PS64)?     %lu", pte & (1 << 8));
    rvvm_info("PTE: flag (RW)?       %lu", pte & (1 << 1));
    rvvm_info("PTE: flag (present)?  %lu", pte & (1 << 0));

    if (!(pte & 1)) {
        rvvm_warn("PTE 0x%lx is invalid!", pte);
        return (xe2_dma_addr_t) {
            .addr = 0,
            .type = 0
        };
    }

    return (xe2_dma_addr_t) {
        .addr = (pte & 0x0000FFFFFFFFF000ULL) + off,
        .type = (pte & 2) ? XE2_MEM_LMEM : XE2_MEM_SMEM
    };
}

static uint32_t xe2_dma_read32(xe2_dev_t *xe2, xe2_dma_addr_t dma, size_t off)
{
    if (dma.type == XE2_MEM_LMEM) {
        if (dma.addr + off + 4 > XE2_VRAM_SIZE)
            return 0;
        return read_uint32_le(xe2->vram + dma.addr + off);
    } else {
        uint32_t *ptr = pci_get_dma_ptr(xe2->pci_func, dma.addr + off, 4);
        return ptr ? *ptr : 0;
    }
}

static void xe2_dma_write32(xe2_dev_t *xe2, xe2_dma_addr_t dma, size_t off, uint32_t msg)
{
    if (dma.type == XE2_MEM_LMEM) {
        if (unlikely(dma.addr + off + 4 > XE2_VRAM_SIZE)) {
            rvvm_info("%s: Failed", __FUNCTION__);
            return;
        }
        write_uint32_le(xe2->vram + dma.addr + off, msg);
        rvvm_info("%s: write_uint32_le[0x%lx]: 0x%x done", __FUNCTION__, dma.addr + off, msg);
    } else {
        uint32_t *ptr = pci_get_dma_ptr(xe2->pci_func, dma.addr + off, 4);
        if (likely(ptr)) {
            *ptr = msg;
        }
        rvvm_info("%s: DMA done", __FUNCTION__);
    }
}

static inline bool xe2_guc_klv_address_key(uint32_t key)
{
    switch (key) {
        case GUC_KLV_SELF_CFG_MEMIRQ_STATUS_ADDR_KEY:
        case GUC_KLV_SELF_CFG_MEMIRQ_SOURCE_ADDR_KEY:
        case GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY:
        case GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY:
        case GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY:
        case GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY:
            return 1;
        default:
            return 0;
    }
}

static inline uint32_t xe2_guc_action_self_cfg(xe2_dev_t *xe2, uint32_t *actions)
{
    uint32_t key = xe2_reg_field_get(xe2_reg_genmask(31, 16), actions[1]);
    rvvm_info("GUC action (address key)?  %d", xe2_guc_klv_address_key(key));

    uint32_t response = (1 << 31)  // GUC_HXG_ORIGIN_GUC
                      | (7 << 28)  // GUC_HXG_TYPE_RESPONSE_SUCCESS
                      | (1 <<  0); // AUX data (1)

    uint64_t value = (uint64_t) actions[2]
                   | (uint64_t) actions[3] << 32;
    xe2_dma_addr_t dma_addr = xe2_guc_klv_address_key(key)
        ? xe2_ggtt_translate(xe2, value)
        : (xe2_dma_addr_t) {0};

    if (xe2_guc_klv_address_key(key) && !dma_addr.addr) {
        rvvm_warn("GGTT returned NULL: (dma_addr: 0x%lx, addr: 0x%lx)", dma_addr.addr, value);
        return response;
    }

    switch (key) {
        case GUC_KLV_SELF_CFG_MEMIRQ_STATUS_ADDR_KEY:
            xe2->guc.memirq_status_addr = dma_addr;
            rvvm_info("GuC cfg: memirq status addr: 0x%lx", xe2->guc.memirq_status_addr.addr);
            break;

        case GUC_KLV_SELF_CFG_MEMIRQ_SOURCE_ADDR_KEY:
            xe2->guc.memirq_source_addr = dma_addr;
            rvvm_info("GuC cfg: memirq source addr: 0x%lx", xe2->guc.memirq_source_addr.addr);
            break;

        case GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY:
            xe2->guc.ctb_h2g_addr = dma_addr;
            rvvm_info("GuC cfg: H2G CTB addr: 0x%lx", xe2->guc.ctb_h2g_addr.addr);
            break;

        case GUC_KLV_SELF_CFG_H2G_CTB_SIZE_KEY:
            rvvm_info("GuC cfg: H2G CTB size: %lu", value);
            xe2->guc.ctb_h2g_size = value;
            break;

        case GUC_KLV_SELF_CFG_G2H_CTB_SIZE_KEY:
            rvvm_info("GuC cfg: G2H CTB size: %lu", value);
            xe2->guc.ctb_g2h_size = value;
            break;

        case GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY:
            xe2->guc.ctb_h2g_descriptor_addr = dma_addr;
            rvvm_info("GuC cfg: H2G CTB descriptor addr: 0x%lx", xe2->guc.ctb_h2g_descriptor_addr.addr);
            break;

        case GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY:
            xe2->guc.ctb_g2h_addr = dma_addr;
            rvvm_info("GuC cfg: G2H CTB addr: 0x%lx", xe2->guc.ctb_g2h_addr.addr);
            break;

        case GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY:
            xe2->guc.ctb_g2h_descriptor_addr = dma_addr;
            rvvm_info("GuC cfg: G2H CTB descriptor addr: 0x%lx", xe2->guc.ctb_g2h_descriptor_addr.addr);
            break;

        default:
            break;
    }

    return response;
}

// GuC commands pipeline:
//
// write (GUC FW SW 1): 32 bit header
// write (GUC FW SW 2): 32 bit payload
// write (GUC FW SW 3): 32 bit payload
// write (GUC FW SW 4): 32 bit payload
static inline void xe2_guc_action(xe2_dev_t *xe2, uint32_t *h2g, uint32_t *g2h)
{
    uint32_t arg = 0U;

    // GuC reports addresses of CTB, CTB descriptor (for both directions)
    rvvm_info("GUC action (request):      %08x", h2g[0]);
    rvvm_info("GUC action (key | len):    %08x", h2g[1]);
    rvvm_info("GUC action (value hi):     %08x", h2g[2]);
    rvvm_info("GUC action (value lo):     %08x", h2g[3]);

    switch (h2g[0]) {
        case XE2_GUC_ACTION_GET_HWCONFIG: // 0x4100
            arg = 0x1000;
            break;
        case XE2_GUC_ACTION_HOST2GUC_SELF_CFG: { // 0x508
            arg = xe2_guc_action_self_cfg(xe2, h2g);
            break;
        }
        case XE2_GUC_ACTION_HOST2GUC_CONTROL_CTB: // 0x4509
            arg = 0; // GUC_CTB_CONTROL_ENABLE
            break;
        case XE2_GUC_ACTION_OPT_IN_FEATURE_KLV: // 0x550E
            arg = 1;
            break;
        default:
            break;
    }

    uint32_t cmd = xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_ORIGIN_MASK, 1) // Origin GUC
                 | xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_TYPE_MASK, 7)   // Success
                 | xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_DATA_MASK, arg);

    g2h[0] = cmd;

    rvvm_info(" ");
}

// Read/write one dword in a CTB ring; head/tail are dword indices that wrap
// within the buffer.
static inline uint32_t xe2_ctb_get(xe2_dev_t *xe2, xe2_dma_addr_t buf, uint32_t idx, uint32_t dwords)
{
    return xe2_dma_read32(xe2, buf, (idx % dwords) * 4);
}

static inline void xe2_ctb_put(xe2_dev_t *xe2, xe2_dma_addr_t buf, uint32_t idx, uint32_t dwords, uint32_t val)
{
    xe2_dma_write32(xe2, buf, (idx % dwords) * 4, val);
}

// Frame an HXG message into the G2H ring as [CTB header][n payload dwords],
// advance the producer tail and raise the GuC interrupt. 'fence' echoes the
// originating request so a blocking transport send completes.
static void xe2_guc_g2h_push(xe2_dev_t *xe2, uint32_t fence, const uint32_t *hxg, uint32_t n)
{
    uint32_t dwords = xe2->guc.ctb_g2h_size / 4;
    if (dwords == 0) {
        return;
    }

    uint32_t tail = xe2_dma_read32(xe2, xe2->guc.ctb_g2h_descriptor_addr, 4);

    uint32_t header = xe2_reg_field_prep(XE2_GUC_CTB_MSG_0_FENCE, fence)
                    | xe2_reg_field_prep(XE2_GUC_CTB_MSG_0_FORMAT, XE2_GUC_CTB_FORMAT_HXG)
                    | xe2_reg_field_prep(XE2_GUC_CTB_MSG_0_NUM_DWORDS, n);

    xe2_ctb_put(xe2, xe2->guc.ctb_g2h_addr, tail, dwords, header);
    for (uint32_t i = 0; i < n; i++) {
        xe2_ctb_put(xe2, xe2->guc.ctb_g2h_addr, tail + XE2_GUC_CTB_HDR_LEN + i, dwords, hxg[i]);
    }

    tail = (tail + XE2_GUC_CTB_HDR_LEN + n) % dwords;
    xe2_dma_write32(xe2, xe2->guc.ctb_g2h_descriptor_addr, 4, tail);

    pci_send_irq(xe2->pci_func, 0);
}

// Reply to a transport request with a single-dword success response.
static void xe2_guc_g2h_response(xe2_dev_t *xe2, uint32_t fence, uint32_t data0)
{
    uint32_t hxg = xe2_reg_field_prep(XE2_GUC_HXG_MSG_0_ORIGIN, XE2_GUC_HXG_ORIGIN_GUC)
                 | xe2_reg_field_prep(XE2_GUC_HXG_MSG_0_TYPE, XE2_GUC_HXG_TYPE_RESPONSE_SUCCESS)
                 | xe2_reg_field_prep(XE2_GUC_HXG_RESPONSE_MSG_0_DATA0, data0);
    xe2_guc_g2h_push(xe2, fence, &hxg, 1);
}

// Read a dword from the registered context's LRC register state.
static uint32_t xe2_lrc_ctx_reg(xe2_dev_t *xe2, uint32_t idx)
{
    return xe2_dma_read32(xe2, xe2->pphwsp_addr, XE2_LRC_REGS_OFFSET + idx * 4);
}

// Perform a ring post-sync store: write a value to a GGTT address.
static void xe2_ring_store(xe2_dev_t *xe2, uint32_t ggtt_addr, uint32_t value)
{
    if (ggtt_addr == 0) {
        return;
    }
    xe2_dma_addr_t dst = xe2_ggtt_translate(xe2, ggtt_addr);
    if (dst.addr == 0) {
        return;
    }
    xe2_dma_write32(xe2, dst, 0, value);
}

// Walk the LRC ring between HEAD and TAIL and execute the post-sync seqno
// stores the job appended (MI_STORE_DATA_IMM / MI_FLUSH_DW / PIPE_CONTROL with a
// GGTT write). We do not run the batch itself; only its completion postamble has
// observable side effects the driver waits on (the seqno reaching the fence
// value). Returns true if a user interrupt was found in the stream.
static bool xe2_ring_replay(xe2_dev_t *xe2)
{
    uint32_t ring_ggtt = xe2_lrc_ctx_reg(xe2, XE2_CTX_RING_START);
    uint32_t head      = xe2_lrc_ctx_reg(xe2, XE2_CTX_RING_HEAD);
    uint32_t tail      = xe2_lrc_ctx_reg(xe2, XE2_CTX_RING_TAIL);
    uint32_t ctl       = xe2_lrc_ctx_reg(xe2, XE2_CTX_RING_CTL);

    if (ring_ggtt == 0 || head == tail) {
        return false;
    }

    xe2_dma_addr_t ring = xe2_ggtt_translate(xe2, ring_ggtt);
    if (ring.addr == 0) {
        return false;
    }

    // RING_CTL holds (size - PAGE_SIZE) page-aligned; recover the dword count.
    uint32_t ring_bytes = (ctl & 0x003ff000U) + 0x1000U;
    uint32_t ring_dw    = ring_bytes / 4;
    bool     user_int   = false;

    uint32_t i = (head / 4) % ring_dw;
    uint32_t end = (tail / 4) % ring_dw;

    for (uint32_t guard = 0; i != end && guard < ring_dw; guard++) {
        uint32_t h   = xe2_dma_read32(xe2, ring, i * 4);
        uint32_t len = 1;

        if (XE2_INSTR_TYPE(h) == XE2_INSTR_TYPE_MI) {
            switch (XE2_MI_OPCODE(h)) {
                case XE2_MI_OP_NOOP:
                case XE2_MI_OP_ARB_CHECK:
                case XE2_MI_OP_ARB_ON_OFF:
                case XE2_MI_OP_BATCH_BUFFER_END:
                    len = 1;
                    break;
                case XE2_MI_OP_USER_INTERRUPT:
                    len = 1;
                    user_int = true;
                    break;
                case XE2_MI_OP_STORE_DATA_IMM:
                    len = (h & 0x3ff) + 2;
                    if (h & XE2_MI_SDI_GGTT) {
                        uint32_t a = xe2_dma_read32(xe2, ring, ((i + 1) % ring_dw) * 4);
                        uint32_t v = xe2_dma_read32(xe2, ring, ((i + 3) % ring_dw) * 4);
                        xe2_ring_store(xe2, a, v);
                    }
                    break;
                case XE2_MI_OP_FLUSH_DW:
                    len = (h & 0x3f) + 2;
                    if (h & XE2_MI_FLUSH_DW_OP_STOREDW) {
                        uint32_t a = xe2_dma_read32(xe2, ring, ((i + 1) % ring_dw) * 4);
                        uint32_t v = xe2_dma_read32(xe2, ring, ((i + 3) % ring_dw) * 4);
                        if (a & XE2_MI_FLUSH_DW_USE_GTT) {
                            xe2_ring_store(xe2, a & ~0x7U, v);
                        }
                    }
                    break;
                default:
                    len = (h & 0xff) + 2;
                    break;
            }
        } else if (XE2_INSTR_TYPE(h) == XE2_INSTR_TYPE_GFXPIPE
                   && (h >> 24) == XE2_PIPE_CONTROL_SIG) {
            len = (h & 0xff) + 2;
            uint32_t flags = xe2_dma_read32(xe2, ring, ((i + 1) % ring_dw) * 4);
            if ((flags & XE2_PIPE_CONTROL_QW_WRITE)
                && (flags & XE2_PIPE_CONTROL_GLOBAL_GTT)) {
                uint32_t a = xe2_dma_read32(xe2, ring, ((i + 2) % ring_dw) * 4);
                uint32_t v = xe2_dma_read32(xe2, ring, ((i + 4) % ring_dw) * 4);
                xe2_ring_store(xe2, a, v);
            }
        } else {
            len = (h & 0xff) + 2;
        }

        if (len == 0) {
            len = 1;
        }
        i = (i + len) % ring_dw;
    }

    // The ring is now drained; advance HEAD so the next submission is isolated.
    xe2_dma_write32(xe2, xe2->pphwsp_addr,
                    XE2_LRC_REGS_OFFSET + XE2_CTX_RING_HEAD * 4, tail);
    return user_int;
}

// Complete render-engine work. Replay the ring's seqno stores, then post an rcs0
// user interrupt via the memory-based interrupt pages whose GGTT locations the
// driver baked into the LRC register state, so it wakes, reads the seqno and
// signals the job fence.
static void xe2_signal_render_completion(xe2_dev_t *xe2)
{
    if (xe2->pphwsp_addr.addr == 0) {
        return;
    }

    if (!xe2_ring_replay(xe2)) {
        return;
    }

    uint32_t src_ggtt = xe2_lrc_ctx_reg(xe2, XE2_CTX_INT_SRC_REPORT_PTR);
    uint32_t sts_ggtt = xe2_lrc_ctx_reg(xe2, XE2_CTX_INT_STATUS_REPORT_PTR);
    if (src_ggtt == 0 || sts_ggtt == 0) {
        return;
    }

    // Render source byte: its IIR bit in the engine's source-report page.
    xe2_dma_addr_t src = xe2_ggtt_translate(xe2, src_ggtt);
    xe2_dma_write32(xe2, src, XE2_MEMIRQ_RENDER_SRC_BYTE, XE2_MEMIRQ_BYTE_SET);

    // Render user-interrupt byte: byte 0 of the engine's status vector.
    xe2_dma_addr_t sts = xe2_ggtt_translate(xe2, sts_ggtt);
    xe2_dma_write32(xe2, sts, XE2_MEMIRQ_RENDER_STATUS_BYTE, XE2_MEMIRQ_BYTE_SET);

    pci_send_irq(xe2->pci_func, 0);
}

// GuC Command Transport: the driver writes H2G requests into a circular ring
// and, for blocking sends, waits for a G2H response that echoes the request's
// fence. Drain every pending H2G message, act on it, and push a fence-matched
// success response for each request.
static void xe2_guc_host_interrupt(xe2_dev_t *xe2)
{
    // Scratch-register (MMIO) transport path, used before the CT rings are up.
    xe2_guc_action(xe2, xe2->guc.actions_h2g, xe2->guc.actions_g2h);

    uint32_t dwords = xe2->guc.ctb_h2g_size / 4;
    if (dwords == 0) {
        return;
    }

    uint32_t head = xe2_dma_read32(xe2, xe2->guc.ctb_h2g_descriptor_addr, 0);
    uint32_t tail = xe2_dma_read32(xe2, xe2->guc.ctb_h2g_descriptor_addr, 4);

    // A doorbell carrying no host-to-GuC message is a pure ring-work submission;
    // a doorbell that delivers CT requests must not also be treated as one, or
    // the spurious completion corrupts the in-flight CT exchange.
    bool ct_message = (head != tail);

    while (head != tail) {
        uint32_t header     = xe2_ctb_get(xe2, xe2->guc.ctb_h2g_addr, head, dwords);
        uint32_t num_dwords = xe2_reg_field_get(XE2_GUC_CTB_MSG_0_NUM_DWORDS, header);
        uint32_t fence      = xe2_reg_field_get(XE2_GUC_CTB_MSG_0_FENCE, header);

        if (num_dwords == 0) {
            break;
        }

        uint32_t msg[64] = {0};
        for (uint32_t i = 0; i < num_dwords && i < 64; i++) {
            msg[i] = xe2_ctb_get(xe2, xe2->guc.ctb_h2g_addr, head + XE2_GUC_CTB_HDR_LEN + i, dwords);
        }

        uint32_t type   = xe2_reg_field_get(XE2_GUC_HXG_MSG_0_TYPE, msg[0]);
        uint32_t action = xe2_reg_field_get(XE2_GUC_HXG_MSG_0_ACTION, msg[0]);

        rvvm_info("GuC CT: action=0x%x type=%u fence=0x%x dwords=%u", action, type, fence, num_dwords);

        switch (action) {
            case XE2_GUC_ACTION_REGISTER_CONTEXT: {
                // The registered context address points at the per-process HW
                // status page (PPHWSP), which is the first page of the context.
                rvvm_addr_t hwlrca = (rvvm_addr_t) msg[10]
                                   | (rvvm_addr_t) msg[11] << 32;
                hwlrca &= 0x0000FFFFFFFFF000ULL; // page addr only; drop desc flags + engine class/instance
                xe2->hwlrca_addr = xe2_ggtt_translate(xe2, hwlrca);
                xe2->pphwsp_addr = xe2_ggtt_translate(xe2, hwlrca);
                rvvm_info("GuC CT: register context, PPHWSP 0x%llx -> 0x%llx",
                          (uint64_t) hwlrca, (uint64_t) xe2->pphwsp_addr.addr);
                // The first registered context is rcs0 (irq_page 0); its source
                // pointer reveals the shared memirq BO base for GuC signalling.
                if (xe2->guc.memirq_base_ggtt == 0) {
                    uint32_t src = xe2_lrc_ctx_reg(xe2, XE2_CTX_INT_SRC_REPORT_PTR);
                    if (src > XE2_MEMIRQ_SOURCE_PAGE_OFFSET) {
                        xe2->guc.memirq_base_ggtt = src - XE2_MEMIRQ_SOURCE_PAGE_OFFSET;
                    }
                }
                break;
            }
            default:
                break;
        }

        // A request expects a fence-matched response; an event does not.
        if (type == XE2_GUC_HXG_TYPE_REQUEST) {
            xe2_guc_g2h_response(xe2, fence, 0);
        }

        head = (head + XE2_GUC_CTB_HDR_LEN + num_dwords) % dwords;
    }

    // Publish the updated consumer head.
    xe2_dma_write32(xe2, xe2->guc.ctb_h2g_descriptor_addr, 0, head);

    // A pure submission doorbell (no CT traffic) stands in for ring work;
    // complete it so the job fence signals.
    if (!ct_message) {
        xe2_signal_render_completion(xe2);
    }
}

static inline void xe2_dpcd_aux_config(uint32_t cmd, uint32_t request, uint32_t size, xe2_aux_t *aux)
{
    // Outgoing AUX request layout:
    // [0]: [********........................]
    //      31  ACK | data                   0
    //
    // [1]: [................................]
    //      31        data                   0
    //
    // [2]: [................................]
    //      31        data                   0
    //
    // [3]: [................................]
    //      31        data                   0
    //
    // [4]: [........************************]
    //      31 data | unused                 0
    //
    // 16 bytes total.
    switch (cmd) {
        case DPCD_REG_REV:
            aux->data[0] = 0x13 << 16; // DPCD rev 13
            break;
        case DPCD_REG_RECEIVER_ALPM_CAP:
            aux->data[0] = 0x01 << 16; // DP_ALPM_CAP
            break;
        case DPCD_REG_DSC_SUPPORT:
            aux->data[0] = 0x03 << 16; // DP_DSC_DECOMPRESSION_IS_SUPPORTED & DP_DSC_PASSTHROUGH_IS_SUPPORTED
            break;
        case DPCD_REG_PSR_SUPPORT:
            aux->data[0] = 0x01 << 16; // DP_PSR_IS_SUPPORTED
            break;
        case DPCD_REG_PANEL_REPLAY_CAP_SUPPORT:
            aux->data[0] = 0x01 << 16; // DP_PANEL_REPLAY_SUPPORT
            break;
        case DPCD_REG_SOURCE_OUI:
            aux->data[0] = 0xAA01 << 8; // Probably hardcoded value
            break;
        case DPCD_INTEL_EDID_ADDR: {
            uint32_t header  = read_uint32_be_m(&xe2_edid[aux->edid_written +  0]) >> 8;
            uint32_t chunk_1 = read_uint32_be_m(&xe2_edid[aux->edid_written +  3]);
            uint32_t chunk_2 = read_uint32_be_m(&xe2_edid[aux->edid_written +  7]);
            uint32_t chunk_3 = read_uint32_be_m(&xe2_edid[aux->edid_written + 11]);
            uint32_t chunk_4 = read_uint8(&xe2_edid[aux->edid_written + 15]) << 24;

            switch (request) {
                case DPCD_REQ_I2C_WRITE_MOT:
                    // When driver writes I2C-over-AUX, it expects this kind of ACK,
                    // which is different from read ACK (upper 8 bits = 0x00).
                    write_uint32_le(&aux->data[0], xe2_reg_field_prep(xe2_reg_genmask(23, 0), 0x110000));
                    // This represents sequenced interface. We can assume that
                    // I2C write-MOT could serve as reset condition, despite this
                    // is not formally defined in DisplayPort IP.
                    aux->edid_written = 0;
                    break;
                case DPCD_REQ_I2C_READ_MOT:
                    if (size == 16 && aux->edid_written < (sizeof(xe2_edid) - 16)) {
                        aux->edid_written += 16;
                    }
                    write_uint32_le(&aux->data[0], header);
                    break;
                default:
                    write_uint32_le(&aux->data[0], header);
                    break;
            }

            write_uint32_le(&aux->data[1], chunk_1);
            write_uint32_le(&aux->data[2], chunk_2);
            write_uint32_le(&aux->data[3], chunk_3);
            write_uint32_le(&aux->data[4], chunk_4);
            break;
        }
        default:
            // Unhandled command. No reason to emit error or warning.
            aux->data[0] = 0x00;
            break;
    }
}

static inline void xe2_emulate_aux_transfer(xe2_dev_t *xe2, size_t aux_no)
{
    xe2_aux_t *aux = &xe2->aux[aux_no];
    uint32_t   cmd = aux->data[0];

    uint32_t request = xe2_reg_field_get(xe2_reg_genmask(31, 28), cmd);
    uint32_t address = xe2_reg_field_get(xe2_reg_genmask(27,  8), cmd);
    uint32_t size    = xe2_reg_field_get(xe2_reg_genmask( 4,  0), cmd) + 2;
    // Linux manipulates with AUX transfer size taking header (1 byte)
    // into the account. Finally, GPU returns size equal len(payload) + 2(headers).
    uint32_t payload_size   = size - 1;

    xe2->aux[0].ctl &= ~xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_MSG_SIZE_MASK, 0xF);
    xe2->aux[0].ctl |=  xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_MSG_SIZE_MASK, size);

    xe2_dpcd_aux_config(address, request, payload_size, aux);
}

static inline bool xe2_skip_mmio_range(size_t offset)
{
    bool skip = 0;
    skip |= offset >= 0x050000 && offset <= 0x05FFFF;
    skip |= offset >= 0x090000 && offset <= 0x09FFFF;
    skip |= offset >= 0x800000 && offset <= 0x8FFFFF;
    skip |= offset >= 0x900000 && offset <= 0x9FFFFF;
    skip |= offset >= 0xA00000 && offset <= 0xAFFFFF;
    skip |= offset >= 0xB00000 && offset <= 0xBFFFFF;
    skip |= offset >= 0xC00000 && offset <= 0xCFFFFF;
    skip |= offset >= 0xD00000 && offset <= 0xDFFFFF;
    skip |= offset >= 0xE00000 && offset <= 0xEFFFFF;
    skip |= offset >= 0xF00000 && offset <= 0xFFFFFF;
    skip |= offset == XE2_REG_FLUSH_PENDING;
    skip |= offset == XE2_REG_GT_GMD_ID;
    skip |= offset == XE2_REG_FLUSH_PENDING;
    skip |= offset == XE2_REG_PRIMARY_SPI_ADDRESS;
    skip |= offset == XE2_REG_PRIMARY_SPI_TRIGGER;
    return skip;
}

static bool xe2_mmio_read(rvvm_mmio_dev_t *dev, void *data, size_t offset, uint8_t size)
{
    UNUSED(size);

    xe2_dev_t *xe2 = dev->data;
    spin_lock(&xe2->lock);

    if (!xe2_skip_mmio_range(offset))
        rvvm_info("PCI read: offset=%lx, data=%x", offset, read_uint32_le(data));

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
        case 0x380000 + XE2_REG_GT_GMD_ID_DISPLAY: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GT_GMD_ID_DISPLAY_ARCH_MASK, 14)
                         | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_DISPLAY_RELEASE_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_GT_GMD_ID_DISPLAY_REVID_MASK, 0);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_PCODE_MAILBOX: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_PCODE_MAILBOX_READY_MASK, 0);
            cmd |= 0x0;
            rvvm_info("PCODE error mask: %x", cmd & 0xFF);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_PCODE_DATA0:
            write_uint32_le(data, 0xFF);
            break;
        case XE2_REG_PCODE_DATA1:
            write_uint32_le(data, 0xFF);
            break;

        case XE2_REG_PCODE_SCRATCH(0): {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_PCODE_SCRATCH_BOOT_STATUS_MASK, 0);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GU_CNTL: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GU_CNTL_LMEM_INIT_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_PRIMARY_SPI_TRIGGER:
            write_uint32_le(data, xe2->spi_trigger);
            break;
        case XE2_REG_PRIMARY_SPI_ADDRESS:
            write_uint32_le(data, xe2->spi_address);
            break;
        case XE2_REG_GT_FORCEWAKE_ACK_GSC:
            write_uint32_le(data, xe2->forcewake_gsc);
            break;
        case XE2_REG_GT_FORCEWAKE_ACK_GT_MTL:
        case XE2_REG_GT_FORCEWAKE_ACK_GT:
            write_uint32_le(data, xe2->forcewake_gt_mtl);
            break;
        case XE2_REG_GT_FORCEWAKE_ACK_RENDER:
            write_uint32_le(data, xe2->forcewake_renderer);
            break;
        case XE2_REG_GT_GDRST:
            xe2->gt_gdrst = 0;
            write_uint32_le(data, xe2->gt_gdrst);
            break;

        case XE2_REG_GSMBASE_LO:
            write_uint32_le(data, 0x00000000);
            break;
        case XE2_REG_GSMBASE_HI:
            write_uint32_le(data, 0x00000001);
            break;
        case XE2_REG_DSMBASE_LO:
            write_uint32_le(data, 0x00800000); // DSMBASE = GSMBASE + 8MB
            break;
        case XE2_REG_DSMBASE_HI:
            write_uint32_le(data, 0x00000001);
            break;

        case XE2_REG_XEHP_TILE_ADDRESS_RANGE(0): {
            uint32_t cmd = xe2_reg_field_prep(xe2_reg_genmask(14, 8), 0x40)  // Tile size
                         | xe2_reg_field_prep(xe2_reg_genmask( 7, 1), 0x00); // Tile offset
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_XELP_GT_GEOMETRY_DSS_ENABLE:
            write_uint32_le(data, 0x1);
            break;
        case XE2_REG_XELP_XEHP_GT_COMPUTE_DSS_ENABLE:
            write_uint32_le(data, 0x1);
            break;
        case XE2_REG_XELP_EU_ENABLE:
            write_uint32_le(data, 0xFFFF);
            break;
        case XE2_REG_MIRROR_FUSE3: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_MIRROR_FUSE3_MEML3_EN_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_MIRROR_FUSE3_NODE_ENABLE_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_MIRROR_FUSE3_XEHPC_GT_L3_MODE_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }

        case XE2_REG_STOLEN_RESERVED_LO:
            write_uint32_le(data, 0x1000);
            break;
        case XE2_REG_STOLEN_RESERVED_HI:
            write_uint32_le(data, 0x0);
            break;

        case XE2_REG_PP_STATUS:
            write_uint32_le(data, xe2->pp_status);
            break;
        case XE2_REG_PP_CONTROL:
            xe2->pp_control |= xe2_reg_field_prep(XE2_REG_PP_CONTROL_POWER_ON_MASK, 1)
                            |  xe2_reg_field_prep(XE2_REG_PP_CONTROL_EPD_FORCE_VDD_MASK, 1);
            write_uint32_le(data, xe2->pp_control);
            break;
        case XE2_REG_PP_ON_DELAYS:
            write_uint32_le(data, xe2->pp_on_delays);
            break;
        case XE2_REG_PP_OFF_DELAYS:
            write_uint32_le(data, xe2->pp_off_delays);
            break;

        case XE2_REG_PCH_PP_STATUS:
            write_uint32_le(data, xe2->pp_status);
            break;
        case XE2_REG_PCH_PP_CONTROL:
            xe2->pp_control |= xe2_reg_field_prep(XE2_REG_PP_CONTROL_POWER_ON_MASK, 1)
                            |  xe2_reg_field_prep(XE2_REG_PP_CONTROL_EPD_FORCE_VDD_MASK, 1);
            write_uint32_le(data, xe2->pp_control);
            break;
        case XE2_REG_PCH_PP_ON_DELAYS:
            write_uint32_le(data, xe2->pp_on_delays);
            break;
        case XE2_REG_PCH_PP_OFF_DELAYS:
            write_uint32_le(data, xe2->pp_off_delays);
            break;

        case XE2_REG_HSW_POWER_WELL_CTL1:
        case XE2_REG_HSW_POWER_WELL_CTL2:
        case XE2_REG_HSW_POWER_WELL_CTL3:
        case XE2_REG_HSW_POWER_WELL_CTL4:
            write_uint32_le(data, 0xFFFFFFFF);
            break;
        case XE2_REG_BXT_DE_PLL_ENABLE: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_BXT_DE_PLL_ENABLE_LOCK_MASK, xe2->pll_enable);
            write_uint32_le(data, cmd);
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
                         | xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_MASK, 2);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO(0): {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_DCLK_MASK, 3200)
                         | xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_TRCD_MASK, 32)
                         | xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_TRP_MASK, 32);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI(0): {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI_TRAS_MASK, 64)
                         | xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI_TRDPRE_MASK, 16);
            write_uint32_le(data, cmd);
            break;
        }

        case XE2_REG_GU_CNTL_PROTECTED: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GU_CNTL_PROTECTED_PRESENT_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_SKL_DSSM:
            write_uint32_le(data, XE2_REG_SKL_DSSM_PLL_REFCLK_38_4MHZ);
            break;
        case XE2_REG_CDCLK_CTL: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_CDCLK_CTL_FREQ_SEL_MASK, 2)     // 337/308 freq
                         | xe2_reg_field_prep(XE2_REG_CDCLK_CTL_SOURCE_SEL_MASK, 1)   // CDCDLK_PLL source select
                         | xe2_reg_field_prep(XE2_REG_CDCLK_CTL_CD2X_DIV_SEL_MASK, 0) // 1x divisor
                         | xe2_reg_field_prep(XE2_REG_CDCLK_CTL_CD2X_PIPE_MASK, 3);   // No pipe
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_DC_STATE_EN:
            write_uint32_le(data, xe2->dc_state);
            break;
        case XE2_REG_ILK_DPFC_CONTROL_1:
        case XE2_REG_ILK_DPFC_CONTROL_2: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_ILK_DPFC_CONTROL_X_EN_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GUC_STATUS: {
            // GuC (Graphics μcontroller) has read-only registers determining whether
            // this GPU component initialized or not.
            //
            // We need more complex GuC logic than hardcode.
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GUC_STATUS_MIA_IN_RESET_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_GUC_STATUS_BOOTROM_MASK, 0)
                         | xe2_reg_field_prep(XE2_REG_GUC_STATUS_UKERNEL_MASK, 0xF0); // enum xe_guc_load_status
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GUC_FW_SW_1:
            write_uint32_le(data, xe2->guc.actions_g2h[0]);
            break;
        case XE2_REG_GUC_FW_SW_2:
            write_uint32_le(data, xe2->guc.actions_g2h[1]);
            break;
        case XE2_REG_GUC_FW_SW_3:
            write_uint32_le(data, xe2->guc.actions_g2h[2]);
            break;
        case XE2_REG_GUC_FW_SW_4:
            write_uint32_le(data, xe2->guc.actions_g2h[3]);
            break;

        case XE2_REG_GUC_PMTIMESTAMP_LO: {
            write_uint32_le(data, 1779018398);
            break;
        }
        case XE2_REG_GUC_PMTIMESTAMP_HI: {
            write_uint32_le(data, 0);
            break;
        }

        case XE2_REG_GUC_WOPCM_SIZE: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GUC_WOPCM_SIZE_MASK, xe2->wopcm_size)
                         | xe2_reg_field_prep(XE2_REG_GUC_WOPCM_SIZE_LOCKED_MASK, xe2->wopcm_locked);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GUC_WOPCM_OFFSET: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GUC_WOPCM_OFFSET_MASK, xe2->wopcm_offset)
                         | xe2_reg_field_prep(XE2_REG_GUC_WOPCM_OFFSET_VALID, 1)
                         | xe2_reg_field_prep(XE2_REG_HUC_LOADING_AGENT_GUC_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GUC_DMA_CTRL:
            // START_DMA (bit 0) reads back 0 once the DMA completes.
            write_uint32_le(data, 0);
            break;
        case 0x8800: {
            write_uint32_le(data, 0x1000);
            break;
        }

        case XE2_REG_STEER_SEMAPHORE:
            write_uint32_le(data, xe2->steer_semaphore);
            break;
        // DPB seems to be unused.
        case XE2_REG_DPA_AUX_CH_CTL: {
            xe2->aux[0].ctl &= ~xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_RECEIVE_ERROR_MASK, 1);
            xe2->aux[0].ctl &= ~xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_TIME_OUT_MASK, 1);
            xe2->aux[0].ctl &= ~xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_TIME_OUT_ERROR_MASK, 1);

            if (xe2->aux[0].ctl & xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_SEND_BUSY_MASK, 1)) {
                xe2->aux[0].ctl &= ~xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_SEND_BUSY_MASK, 1);
                xe2->aux[0].ctl |=  xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_DONE_MASK, 1);
                xe2_emulate_aux_transfer(xe2, 0);
            }
            write_uint32_le(data, xe2->aux[0].ctl);
            break;
        }
        case XE2_REG_DPA_AUX_CH_DATA(0):
        case XE2_REG_DPA_AUX_CH_DATA(1):
        case XE2_REG_DPA_AUX_CH_DATA(2):
        case XE2_REG_DPA_AUX_CH_DATA(3):
        case XE2_REG_DPA_AUX_CH_DATA(4): {
            size_t index = XE2_REG_DPX_AUX_CH_DATA_INDEX(offset);
            write_uint32_le(data, xe2->aux[0].data[index]);
            break;
        }

        // PCI write: offset=80000, data=c0a4040, size = 4
        //
        //
        // There begin cursed decompiled part.
        case XE2_DMC_FW_MAIN_OFFSET:   // DMC program offset
        case XE2_DMC_FW_PIPE_A_OFFSET: // DMC program offset
        case XE2_DMC_FW_PIPE_B_OFFSET: // DMC program offset
        case XE2_DMC_FW_PIPE_C_OFFSET: // DMC program offset
        case XE2_DMC_FW_PIPE_D_OFFSET: // DMC program offset
            write_uint32_le(data, 0xC0A4040); // DMC program size
            break;

        case XE2_REG_DMC_SSP_BASE:
            write_uint32_le(data, xe2->firmware.dmc_base);
            break;

        case XE2_REG_PLANE_CTL_1_A:
        case XE2_REG_PLANE_CTL_2_A:
        case XE2_REG_PLANE_CTL_1_B:
        case XE2_REG_PLANE_CTL_2_B: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_PLANE_CTL_X_ICL_FORMAT_MASK, 14) // RGB565
                         | xe2_reg_field_prep(XE2_REG_PLANE_CTL_X_KEY_ENABLE_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }

        // Hardware engines:
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_BLT_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_RENDER_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_COMPUTE0_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_COMPUTE1_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_COMPUTE2_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_COMPUTE3_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS1_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS2_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS3_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS4_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS5_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS6_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS7_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_GSCCS_RING_BASE): {
            // In kernel: gt->info.timestamp_base = 83333
            //            idledly_units_ps = 8 * gt->info.timestamp_base
            //            idledly = DIV_ROUND_CLOSEST(idledly * idledly_units_ps, 1000)
            //            -> 0xd05
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_HW_ENGINE_RING_IDLEDLY_INHIBIT_SWITCH_UNTIL_PREEMPTED_MASK, 0)
                         | xe2_reg_field_prep(XE2_REG_HW_ENGINE_RING_IDLEDLY_IDLE_DELAY_MASK, 5);
            write_uint32_le(data, cmd);
            break;
        }

        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_BLT_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_RENDER_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_COMPUTE0_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_COMPUTE1_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_COMPUTE2_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_COMPUTE3_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS1_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS2_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS3_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS4_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS5_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS6_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS7_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(XE2_HW_ENGINE_GSCCS_RING_BASE): {
            // In kernel: maxcnt = 10 * 640 (maxcnt_units_ns)
            //            -> 0x1900
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT_IDLE_WAIT_TIME_MASK, 10);
            write_uint32_le(data, cmd);
            break;
        }

        case XE2_REG_HW_ENGINE_RING_HEAD(XE2_HW_ENGINE_RENDER_RING_BASE):
            rvvm_info("Read ring head: HW engine renderer");
            write_uint32_le(data, 0);
            break;

        case XE2_REG_HW_ENGINE_RING_TAIL(XE2_HW_ENGINE_RENDER_RING_BASE):
            rvvm_info("Read ring tail: HW engine renderer");
            write_uint32_le(data, 0);
            break;

        case XE2_REG_HW_ENGINE_RING_HEAD(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE):
            rvvm_info("Read ring head: HW engine BCS8");
            write_uint32_le(data, 0);
            break;

        case XE2_REG_HW_ENGINE_RING_TAIL(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE):
            rvvm_info("Read ring tail: HW engine BCS8");
            write_uint32_le(data, 0);
            break;

        default:
            // For safety initialize all unhandled requests to 0.
            // Note that driver expects zero-initialized interrupt registers
            // at startup (ISR/IMR/IIR/IER).
            write_uint32_le(data, 0x0);
            break;
    }

    // [    6.167743] xe 0000:00:01.0: [drm] DMC 0 mmio[0]/0x8f074 incorrect (expected 0x86fc0, current 0x0)
    if (offset >= XE2_DMC_FW_MAIN_OFFSET && offset + 4 <= XE2_DMC_FW_MAIN_OFFSET + sizeof(xe2->firmware.main)) {
        uint32_t word = read_uint32_le(&xe2->firmware.main[offset - XE2_DMC_FW_MAIN_OFFSET]);
        write_uint32_le(data, word);
    }

    spin_unlock(&xe2->lock);
    return true;
}

static inline bool xe2_ggtt_mmio_range(size_t offset)
{
    size_t begin = XE2_GGTT_MMIO_BASE;
    size_t end   = XE2_GGTT_MMIO_BASE + XE2_GGTT_MMIO_SIZE;

    return offset >= begin && offset <= end;
}

static bool xe2_mmio_write(rvvm_mmio_dev_t *dev, void *data, size_t offset, uint8_t size)
{
    UNUSED(size);

    xe2_dev_t *xe2 = dev->data;
    spin_lock(&xe2->lock);

    if (!xe2_skip_mmio_range(offset))
        rvvm_info("PCI write: offset=%lx, data=%x, size = %u", offset, read_uint32_le(data), size);

    if (xe2_ggtt_mmio_range(offset)) {
        xe2_ggtt_mmio_write(xe2, offset, read_uint32_le(data));
        spin_unlock(&xe2->lock);
        return true;
    }

    switch (offset) {
        case XE2_REG_GT_FORCEWAKE_GSC:
            xe2->forcewake_gsc = read_uint32_le(data);
            break;
        case XE2_REG_GT_FORCEWAKE_GT:
            xe2->forcewake_gt_mtl = read_uint32_le(data);
            break;
        case XE2_REG_GT_FORCEWAKE_RENDER:
            xe2->forcewake_renderer = read_uint32_le(data);
            break;
        case XE2_REG_GT_GDRST:
            xe2->gt_gdrst = read_uint32_le(data);
            break;
        case XE2_REG_STEER_SEMAPHORE:
            xe2->steer_semaphore = read_uint32_le(data);
            break;
        case XE2_REG_PRIMARY_SPI_TRIGGER:
            xe2->spi_trigger = read_uint32_le(data);
            break;
        case XE2_REG_PRIMARY_SPI_ADDRESS:
            xe2->spi_address = read_uint32_le(data);
            break;

        case XE2_REG_DPA_AUX_CH_CTL:
            xe2->aux[0].ctl = read_uint32_le(data);
            break;

        case XE2_REG_DPA_AUX_CH_DATA(0):
        case XE2_REG_DPA_AUX_CH_DATA(1):
        case XE2_REG_DPA_AUX_CH_DATA(2):
        case XE2_REG_DPA_AUX_CH_DATA(3):
        case XE2_REG_DPA_AUX_CH_DATA(4): {
            size_t index = XE2_REG_DPX_AUX_CH_DATA_INDEX(offset);
            xe2->aux[0].data[index] = read_uint32_le(data);
            xe2->aux[0].message_size = ((index + 1) * 4);
            break;
        }

        case XE2_REG_BXT_DE_PLL_ENABLE: {
            uint32_t cmd = read_uint32_le(data);
            xe2->pll_enable = !!xe2_reg_field_get(XE2_REG_BXT_DE_PLL_ENABLE_MASK, cmd);
            break;
        }

        case XE2_REG_PP_STATUS:
            xe2->pp_status = read_uint32_le(data);
            break;
        case XE2_REG_PP_CONTROL:
            xe2->pp_control = read_uint32_le(data);
            break;
        case XE2_REG_PP_ON_DELAYS:
            xe2->pp_on_delays = read_uint32_le(data);
            break;
        case XE2_REG_PP_OFF_DELAYS:
            xe2->pp_off_delays = read_uint32_le(data);
            break;

        case XE2_REG_PCH_PP_STATUS:
            xe2->pp_status = read_uint32_le(data);
            break;
        case XE2_REG_PCH_PP_CONTROL:
            xe2->pp_control = read_uint32_le(data);
            break;
        case XE2_REG_PCH_PP_ON_DELAYS:
            xe2->pp_on_delays = read_uint32_le(data);
            break;
        case XE2_REG_PCH_PP_OFF_DELAYS:
            xe2->pp_off_delays = read_uint32_le(data);
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
        case XE2_REG_DC_STATE_EN: {
            uint32_t cmd = read_uint32_le(data);
            uint32_t mask = XE2_REG_DC_STATE_EN_DC3C0
                          | XE2_REG_DC_STATE_EN_UPTO_DC5;
            xe2->dc_state &= ~mask;
            xe2->dc_state |= (cmd & mask);
            break;
        }

        case XE2_REG_DMC_SSP_BASE:
            xe2->firmware.dmc_base = read_uint32_le(data);
            break;

        case XE2_REG_GUC_WOPCM_SIZE: {
            uint32_t cmd = read_uint32_le(data);
            xe2->wopcm_size = xe2_reg_field_get(XE2_REG_GUC_WOPCM_SIZE_MASK, cmd);
            atomic_store_uint32_relax(&xe2->wopcm_locked, 1);
            break;
        }
        case XE2_REG_GUC_WOPCM_OFFSET: {
            uint32_t cmd = read_uint32_le(data);
            xe2->wopcm_offset = xe2_reg_field_get(XE2_REG_GUC_WOPCM_OFFSET_MASK, cmd);
            break;
        }
        case XE2_REG_GUC_FW_SW_1:
            xe2->guc.actions_h2g[0] = read_uint32_le(data);
            break;
        case XE2_REG_GUC_FW_SW_2:
            xe2->guc.actions_h2g[1] = read_uint32_le(data);
            break;
        case XE2_REG_GUC_FW_SW_3:
            xe2->guc.actions_h2g[2] = read_uint32_le(data);
            break;
        case XE2_REG_GUC_FW_SW_4:
            xe2->guc.actions_h2g[3] = read_uint32_le(data);
            break;

        case XE2_REG_GUC_DMA_ADDR_0_LO:
            xe2->dma_0 = read_uint32_le(data);
            break;
        case XE2_REG_GUC_DMA_ADDR_0_HI: {
            uint32_t cmd = read_uint32_le(data);
            rvvm_addr_t dma = cmd & ~xe2_reg_field_prep(xe2_reg_genmask(20, 16), 0x1F);
            xe2->dma_0 |= dma << 32;
            break;
        }

        case XE2_REG_GUC_TLB_INV_DESC0:
            rvvm_info("GuC requested to invalidate TLB[0]");
            break;
        case XE2_REG_GUC_TLB_INV_DESC1:
            rvvm_info("GuC requested to invalidate TLB[1]");
            break;

        case XE2_REG_GUC_DMA_ADDR_1_LO:
            xe2->dma_1  = (rvvm_addr_t) read_uint32_le(data);
            break;
        case XE2_REG_GUC_DMA_ADDR_1_HI: {
            uint32_t cmd = read_uint32_le(data);
            rvvm_addr_t dma = cmd & ~xe2_reg_field_prep(xe2_reg_genmask(20, 16), 0x1F);
            xe2->dma_1 |= dma << 32;
            break;
        }
        case XE2_REG_GUC_DMA_COPY_SIZE:
            xe2->dma_copy_size = read_uint32_le(data);
            break;

        case XE2_REG_GUC_HOST_INTERRUPT:
            xe2_guc_host_interrupt(xe2);
            break;

        case XE2_REG_HW_ENGINE_RING_HEAD(XE2_HW_ENGINE_RENDER_RING_BASE):
            rvvm_info("Write ring head: HW engine renderer");
            break;

        case XE2_REG_HW_ENGINE_RING_TAIL(XE2_HW_ENGINE_RENDER_RING_BASE):
            rvvm_info("Write ring tail: HW engine renderer");
            break;

        case XE2_REG_HW_ENGINE_RING_HEAD(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE):
            rvvm_info("Write ring head: HW engine BCS8");
            break;

        case XE2_REG_HW_ENGINE_RING_TAIL(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE):
            rvvm_info("Write ring tail: HW engine BCS8");
            break;

        case XE2_REG_GUC_SOFT_SCRATCH( 0):
        case XE2_REG_GUC_SOFT_SCRATCH( 1):
        case XE2_REG_GUC_SOFT_SCRATCH( 2):
        case XE2_REG_GUC_SOFT_SCRATCH( 3):
        case XE2_REG_GUC_SOFT_SCRATCH( 4):
        case XE2_REG_GUC_SOFT_SCRATCH( 5):
        case XE2_REG_GUC_SOFT_SCRATCH( 6):
        case XE2_REG_GUC_SOFT_SCRATCH( 7):
        case XE2_REG_GUC_SOFT_SCRATCH( 8):
        case XE2_REG_GUC_SOFT_SCRATCH( 9):
        case XE2_REG_GUC_SOFT_SCRATCH(10):
        case XE2_REG_GUC_SOFT_SCRATCH(11):
        case XE2_REG_GUC_SOFT_SCRATCH(12):
        case XE2_REG_GUC_SOFT_SCRATCH(13):
        case XE2_REG_GUC_SOFT_SCRATCH(14):
        case XE2_REG_GUC_SOFT_SCRATCH(15): {
            size_t index = XE2_REG_GUC_SOFT_SCRATCH_INDEX(offset);
            rvvm_info("soft scratch write[%lu]: %08x", index, read_uint32_le(data));
            break;
        }

        default:
            break;
    }

    if (offset >= XE2_DMC_FW_MAIN_OFFSET && offset + size <= XE2_DMC_FW_MAIN_OFFSET + sizeof(xe2->firmware.main)) {
        size_t fw_off = offset - XE2_DMC_FW_MAIN_OFFSET;
        memcpy(xe2->firmware.main + fw_off, data, size);
        if (fw_off + size > xe2->firmware.main_loaded)
            xe2->firmware.main_loaded = fw_off + size;
    }

    spin_unlock(&xe2->lock);
    return true;
}

PUBLIC pci_dev_t *xe2_init(pci_bus_t *pci_bus)
{
    xe2_dev_t *xe2 = safe_new_obj(xe2_dev_t);
    xe2->aux[0].edid_written = 0;
    xe2->steer_semaphore = 1; // Begin with unlocked state.
    xe2->vram = vma_alloc(NULL, XE2_VRAM_SIZE, VMA_RDWR);
    xe2->ggtt_pte = vma_alloc(NULL, XE2_GGTT_PAGES * sizeof(uint64_t), VMA_RDWR);
    xe2->ggtt_lo_addrs = vma_alloc(NULL, XE2_GGTT_PAGES * sizeof(uint32_t), VMA_RDWR);
    xe2->ggtt_pte_valid = vma_alloc(NULL, XE2_GGTT_PAGES * sizeof(bool), VMA_RDWR);

    pci_func_desc_t xe2_desc = {
        .vendor_id  = XE2_VENDOR_ID_INTEL,
        .device_id  = XE2_DEVICE_ID_ARC_B570_GRAPHICS,
        .class_code = XE2_CLASS_CODE,
        .prog_if    = 0,
        .irq_pin    = PCI_IRQ_PIN_INTA,
        // MMIO + GTT
        .bar[0]     = {
            .size           = 0x1000000,
            .min_op_size    = 1,
            .max_op_size    = 4,
            .read           = xe2_mmio_read,
            .write          = xe2_mmio_write,
            .data           = xe2,
            .type           = &xe2_type
        },
        // VRAM
        .bar[2]         = {
            .size           = XE2_VRAM_SIZE,
            .min_op_size    = 1,
            .max_op_size    = 4,
            .data           = xe2,
            .mapping        = xe2->vram
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
