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
#include "mem_ops.h"
#include "utils.h"
#include <stdint.h>

// MCR (Multicast/Replicated)
// MTL (Meteor Lake)
//
// Current status: RPNSWREQ, RP_CONTROL, RC_CONTROL, RC_STATE needs to be handled.

#define xe2_reg_genmask(h, l)           (((~0U) << (l)) & (~0U >> (31 - (h))))
#define xe2_reg_bit(x)                  xe2_reg_genmask((x), (x))
#define xe2_reg_field_get(mask, val)    (((val) & (mask)) >> __builtin_ctz(mask))
#define xe2_reg_field_prep(mask, val)   (((val) << __builtin_ctz(mask)) & (mask))

#define XE2_VENDOR_ID_INTEL                                 0x8086
#define XE2_DEVICE_ID_LUNAR_LAKE_IGPU                       0x6420
#define XE2_CLASS_CODE                                      0x0300

#define XE2_REG_FLUSH_PENDING                               0x130030 // Dummy register

#define XE2_REG_GT_GMD_ID                                   0xD8C
#define XE2_REG_GT_GMD_ID_ARCH_MASK                         xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_RELEASE_MASK                      xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_REVID_MASK                        xe2_reg_genmask(5, 0)

#define XE2_REG_GT_GMD_ID_DISPLAY                           0x510A0
#define XE2_REG_GT_GMD_ID_DISPLAY_ARCH_MASK                 xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_DISPLAY_RELEASE_MASK              xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_DISPLAY_REVID_MASK                xe2_reg_genmask(5, 0)

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

#define XE2_REG_STEER_SEMAPHORE                             0xFD0
#define XE2_REG_GGC                                         0x108040 // Graphics control
#define XE2_REG_GGC_GMS_MASK                                xe2_reg_genmask(15, 8)
#define XE2_REG_GGC_GGMS_MASK                               xe2_reg_genmask(7, 6)

#define XE2_REG_GUC_TLB_INV_DESC0                           0xCF7C // Write-only for OS
#define XE2_REG_GUC_TLB_INV_DESC1                           0xCF80 // Write-only for OS

#define XE2_REG_GU_CNTL_PROTECTED                           0x10100C // This being used from i915
#define XE2_REG_GU_CNTL_PROTECTED_PRESENT_MASK              xe2_reg_genmask(9, 9)

#define XE2_REG_VF_CAP                                      0x1901F8
#define XE2_REG_VF_CAP_MASK                                 xe2_reg_genmask(0, 0)

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

#define XE2_REG_STOLEN_RESERVED1                            0x1082C0 // Das war schön gestohlen mal...
#define XE2_REG_STOLEN_RESERVED2                            0x1082C4
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

#define XE2_REG_HW_ENGINE_RING_IDLEDLY(base)                (base + 0x23C)
#define XE2_REG_HW_ENGINE_RING_IDLEDLY_INHIBIT_SWITCH_UNTIL_PREEMPTED_MASK \
                                                            xe2_reg_bit(31)
#define XE2_REG_HW_ENGINE_RING_IDLEDLY_IDLE_DELAY_MASK      xe2_reg_genmask(20, 0)

#define XE2_REG_HW_ENGINE_RING_PRWCTX_MAXCNT(base)          (base + 0x54)
#define XE2_REG_HW_ENGINE_RING_PRWCTX_MAXCNT_IDLE_WAIT_TIME_MASK \
                                                            xe2_reg_genmask(19, 0)

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

typedef struct {
    pci_func_t *pci_func;
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

    uint32_t    guc_actions[4];
    xe2_aux_t   aux[1]; // We assume one display with one AUX channel.

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

static void xe2_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t xe2_type = {
    .name = "xe2",
    .remove = xe2_remove,
};

// GuC commands pipeline:
//
// write (GUC FW SW 1): 32 bit header
// write (GUC FW SW 2): 32 bit payload
// write (GUC FW SW 3): 32 bit payload
// write (GUC FW SW 4): 32 bit payload
// ...
// Length of payload depends on header.
//
// write: offset=XE2_REG_GUC_FW_SW_1, data=508,     size = 4    GUC_ACTION_HOST2GUC_SELF_CFG (0x0508)
// write: offset=XE2_REG_GUC_FW_SW_2, data=9030002, size = 4    GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY (0x0903) | len (2 words)
// write: offset=XE2_REG_GUC_FW_SW_3, data=cf7000,  size = 4    Lower 32 bits of payload
// write: offset=XE2_REG_GUC_FW_SW_4, data=0,       size = 4    Upper 32 bits of payload
static inline void xe2_guc_fw_action(void *data, uint32_t action, uint32_t index)
{
    UNUSED(index);

    uint32_t cmd = xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_ORIGIN_MASK, 1) // Origin GUC
                 | xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_TYPE_MASK, 7);  // Success
    uint32_t arg = 0U;

    if (index == 0)
        switch (action) {
            case XE2_GUC_ACTION_GET_HWCONFIG:
                arg = 0x10000;
                break;
            case XE2_GUC_ACTION_HOST2GUC_SELF_CFG:
                arg = 1;
                break;
            default:
                break;
        }

    cmd |= xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_DATA_MASK, arg);

    write_uint32_le(data, cmd);
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
            uint32_t chunk_4 = read_uint32_be_m(&xe2_edid[aux->edid_written + 15]) & 0xFF << 24;

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

static bool xe2_mmio_read(rvvm_mmio_dev_t *dev, void *data, size_t offset, uint8_t size)
{
    UNUSED(size);

    if (offset != XE2_REG_FLUSH_PENDING && offset < 0x80000)
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
        case XE2_REG_GT_FORCEWAKE_ACK_RENDER:
            write_uint32_le(data, xe2->forcewake_renderer);
            break;
        case XE2_REG_GT_GDRST:
            xe2->gt_gdrst = 0;
            write_uint32_le(data, xe2->gt_gdrst);
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
                         | xe2_reg_field_prep(XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_MASK, 1);
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
            xe2_guc_fw_action(data, xe2->guc_actions[0], 0);
            break;
        case XE2_REG_GUC_FW_SW_2:
            xe2_guc_fw_action(data, xe2->guc_actions[1], 1);
            break;
        case XE2_REG_GUC_FW_SW_3:
            xe2_guc_fw_action(data, xe2->guc_actions[2], 2);
            break;
        case XE2_REG_GUC_FW_SW_4:
            xe2_guc_fw_action(data, xe2->guc_actions[3], 3);
            break;

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

        // There begin cursed decompiled part.
        case XE2_DMC_FW_MAIN_OFFSET:   // DMC program offset
        case XE2_DMC_FW_PIPE_A_OFFSET: // DMC program offset
        case XE2_DMC_FW_PIPE_B_OFFSET: // DMC program offset
        case XE2_DMC_FW_PIPE_C_OFFSET: // DMC program offset
        case XE2_DMC_FW_PIPE_D_OFFSET: // DMC program offset
            write_uint32_le(data, 0xC0A4040); // DMC program size
            break;

        case XE2_REG_STOLEN_RESERVED1:
        case XE2_REG_STOLEN_RESERVED2:
            write_uint32_le(data, 0);
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
        case XE2_REG_HW_ENGINE_RING_IDLEDLY(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE): {
            // In kernel: gt->info.timestamp_base = 83333
            //            idledly_units_ps = 8 * gt->info.timestamp_base
            //            idledly = DIV_ROUND_CLOSEST(idledly * idledly_units_ps, 1000)
            //            -> 0xd05
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_HW_ENGINE_RING_IDLEDLY_INHIBIT_SWITCH_UNTIL_PREEMPTED_MASK, 0)
                         | xe2_reg_field_prep(XE2_REG_HW_ENGINE_RING_IDLEDLY_IDLE_DELAY_MASK, 5);
            write_uint32_le(data, cmd);
            break;
        }

        case XE2_REG_HW_ENGINE_RING_PRWCTX_MAXCNT(XE2_HW_ENGINE_BLT_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PRWCTX_MAXCNT(XE2_HW_ENGINE_RENDER_RING_BASE):
        case XE2_REG_HW_ENGINE_RING_PRWCTX_MAXCNT(XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE): {
            // In kernel: maxcnt = 10 * 640 (maxcnt_units_ns)
            //            -> 0x1900
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_HW_ENGINE_RING_PRWCTX_MAXCNT_IDLE_WAIT_TIME_MASK, 10);
            write_uint32_le(data, cmd);
            break;
        }

        default:
            break;
    }

    if (offset >= XE2_DMC_FW_MAIN_OFFSET && offset <= (XE2_DMC_FW_MAIN_OFFSET + sizeof(xe2->firmware.main))) {
        uint32_t word = read_uint32_le(&xe2->firmware.main[offset - XE2_DMC_FW_MAIN_OFFSET]);
        write_uint32_le(data, word);
    }

    return true;
}

static bool xe2_mmio_write(rvvm_mmio_dev_t *dev, void *data, size_t offset, uint8_t size)
{
    UNUSED(size);

    if (offset != XE2_REG_FLUSH_PENDING && offset != XE2_REG_GT_GMD_ID && offset < 0x80000)
        rvvm_info("PCI write: offset=%lx, data=%x, size = %u", offset, read_uint32_le(data), size);

    xe2_dev_t *xe2 = dev->data;

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
            xe2->guc_actions[0] = read_uint32_le(data);
            break;
        case XE2_REG_GUC_FW_SW_2:
            xe2->guc_actions[1] = read_uint32_le(data);
            break;
        case XE2_REG_GUC_FW_SW_3:
            xe2->guc_actions[2] = read_uint32_le(data);
            break;
        case XE2_REG_GUC_FW_SW_4:
            xe2->guc_actions[3] = read_uint32_le(data);
            break;
        default:
            break;
    }

    if (offset >= XE2_DMC_FW_MAIN_OFFSET && offset <= (XE2_DMC_FW_MAIN_OFFSET + sizeof(xe2->firmware.main))) {
        memcpy(xe2->firmware.main + xe2->firmware.main_loaded, data, size);
        xe2->firmware.main_loaded += size;
    }

    return true;
}

PUBLIC pci_dev_t *xe2_init(pci_bus_t *pci_bus)
{
    xe2_dev_t *xe2 = safe_new_obj(xe2_dev_t);
    xe2->aux[0].edid_written = 0;
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
