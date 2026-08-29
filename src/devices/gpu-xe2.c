/*
gpu-xe2.c - Intel XE2 graphics
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <devices/gpu-vulkan-spirv.h>
#include <devices/gpu-vulkan.h>
#include <errno.h>
#include <fcntl.h>
#include <rvvm/rvvm_board.h>
#include <rvvm/rvvm_fb.h>
#include <rvvm/rvvm_pci.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <util/bit_ops.h>
#include <util/compiler.h>
#include <util/mem_ops.h>
#include <util/spinlock.h>
#include <util/utils.h>
#include <util/vma_ops.h>

// Basic DRM programs and weston works with following
// boot arguments:
// - fbcon=map:0 xe.enable_dc=0 xe.enable_dsb=0 xe.disable_power_well=0

/*
XDG setup:

killall -9 Xorg
export XDG_RUNTIME_DIR=/tmp/runtime-$USER
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
weston

killall -9 Xorg
INTEL_DEBUG=bat glmark2-es2-drm
*/

#define xe2_reg_genmask(h, l)                                              (((~0U) << (l)) & (~0U >> (31 - (h))))
#define xe2_reg_genmask64(h, l)                                            (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define xe2_reg_bit(x)                                                     xe2_reg_genmask((x), (x))
#define xe2_reg_field_get(mask, val)                                       (((val) & (mask)) >> bit_ctz64(mask))
#define xe2_reg_field_prep(mask, val)                                      (((val) << bit_ctz64(mask)) & (mask))

#define XE2_VENDOR_ID_INTEL                                                0x8086
#define XE2_DEVICE_ID_ARC_B570_GRAPHICS                                    0xE20C
#define XE2_CLASS_CODE                                                     0x0300

#define XE2_REG_FLUSH_PENDING                                              0x130030 // Dummy register

// GT frequency caps and status. The driver decodes the requested/efficient/
// minimum/maximum ratios from these to bring up dynamic frequency control;
// ratios are in units of 50/3 MHz. Cap values reflect a Battlemage B570.
#define XE2_REG_RP_STATE_CAP                                               0x138000
#define XE2_REG_RP_STATE_CAP_RP0_MASK                                      xe2_reg_genmask(8, 0)
#define XE2_REG_RP_STATE_CAP_RPN_MASK                                      xe2_reg_genmask(24, 16)
#define XE2_REG_GT_RPE_FREQUENCY                                           0x13800C
#define XE2_REG_GT_RPE_FREQUENCY_RPE_MASK                                  xe2_reg_genmask(8, 0)
#define XE2_REG_GT_PERF_STATUS                                             0x1381B4
#define XE2_REG_GT_PERF_STATUS_CAGF_MASK                                   xe2_reg_genmask(19, 11)
#define XE2_REG_RPNSWREQ                                                   0xA008
#define XE2_REG_RPNSWREQ_RATIO_MASK                                        xe2_reg_genmask(31, 23)
#define XE2_REG_RP_CONTROL                                                 0xA024

// Battlemage B570 frequency ratios (raw units of 50/3 MHz): RP0 2500 MHz,
// RPe 2000 MHz, RPn 300 MHz.
#define XE2_GT_FREQ_RP0_RATIO                                              150
#define XE2_GT_FREQ_RPE_RATIO                                              120
#define XE2_GT_FREQ_RPN_RATIO                                              18

#define XE2_REG_PCODE_MAILBOX                                              0x138124
#define XE2_REG_PCODE_MAILBOX_READY_MASK                                   xe2_reg_bit(31)
#define XE2_REG_PCODE_MAILBOX_MB_PARAM2_MASK                               xe2_reg_genmask(23, 16)
#define XE2_REG_PCODE_MAILBOX_MB_PARAM1_MASK                               xe2_reg_genmask(15, 8)
#define XE2_REG_PCODE_MAILBOX_MB_COMMAND_MASK                              xe2_reg_genmask(7, 0)

#define XE2_REG_PCODE_DATA0                                                0x138128
#define XE2_REG_PCODE_DATA1                                                0x13812C

#define XE2_REG_PCODE_SCRATCH(n)                                           (0x138320 + (n) * 4)
#define XE2_REG_PCODE_SCRATCH_AUXINFO_REG_OFFSET_MASK                      xe2_reg_genmask(17, 15)
#define XE2_REG_PCODE_SCRATCH_OVERFLOW_REG_OFFSET_MASK                     xe2_reg_genmask(14, 12)
#define XE2_REG_PCODE_SCRATCH_HISTORY_TRACKING_MASK                        xe2_reg_bit(11)
#define XE2_REG_PCODE_SCRATCH_OVERFLOW_SUPPORT_MASK                        xe2_reg_bit(10)
#define XE2_REG_PCODE_SCRATCH_AUXINFO_SUPPORT_MASK                         xe2_reg_bit(9)
#define XE2_REG_PCODE_SCRATCH_BOOT_STATUS_MASK                             xe2_reg_genmask(3, 1)

#define XE2_REG_GT_GMD_ID                                                  0xD8C
#define XE2_REG_GT_GMD_ID_ARCH_MASK                                        xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_RELEASE_MASK                                     xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_REVID_MASK                                       xe2_reg_genmask(5, 0)

#define XE2_REG_GT_GMD_ID_DISPLAY                                          0x510A0
#define XE2_REG_GT_GMD_ID_DISPLAY_ARCH_MASK                                xe2_reg_genmask(31, 22)
#define XE2_REG_GT_GMD_ID_DISPLAY_RELEASE_MASK                             xe2_reg_genmask(21, 14)
#define XE2_REG_GT_GMD_ID_DISPLAY_REVID_MASK                               xe2_reg_genmask(5, 0)

#define XE2_REG_GU_CNTL                                                    0x101010
#define XE2_REG_GU_CNTL_LMEM_INIT_MASK                                     xe2_reg_bit(7)
#define XE2_REG_GU_CNTL_DRIVERFLR_MASK                                     xe2_reg_bit(31)

#define XE2_REG_PRIMARY_SPI_ADDRESS                                        0x102080
#define XE2_REG_PRIMARY_SPI_TRIGGER                                        0x102040
// SPI flash window the driver scans for the VBT: it picks the static region,
// reads the option-ROM base offset, then latches a byte address and reads back
// the 32-bit flash word. We serve a synthetic VBT image at flash offset 0.
#define XE2_REG_PRIMARY_SPI_REGIONID                                       0x102084
#define XE2_REG_SPI_STATIC_REGIONS                                         0x102090
#define XE2_REG_OROM_OFFSET                                                0x1020C0

#define XE2_REG_GT_FORCEWAKE_GSC                                           0xA618
#define XE2_REG_GT_FORCEWAKE_ACK_GSC                                       0xDF8
#define XE2_REG_GT_FORCEWAKE_GT                                            0xA188
#define XE2_REG_GT_FORCEWAKE_ACK_GT                                        0x130044 // How these forcewakes are related?
#define XE2_REG_GT_FORCEWAKE_ACK_GT_MTL                                    0xDFC
#define XE2_REG_GT_FORCEWAKE_RENDER                                        0xA278
#define XE2_REG_GT_FORCEWAKE_ACK_RENDER                                    0xD84

#define XE2_REG_GT_GDRST                                                   0x941C
#define XE2_REG_GT_GDRST_GRDOM_GUC                                         xe2_reg_bit(3)
#define XE2_REG_GT_GDRST_GRDOM_FULL                                        xe2_reg_bit(0)

#define XE2_REG_GT_POWERGATE_ENABLE                                        0xA210
#define XE2_REG_GT_POWERGATE_ENABLE_RENDER_MASK                            xe2_reg_bit(0)
#define XE2_REG_GT_POWERGATE_ENABLE_MEDIA_MASK                             xe2_reg_bit(1)
#define XE2_REG_GT_POWERGATE_ENABLE_MEDIA_SAMPLES_MASK                     xe2_reg_bit(2)
#define XE2_REG_GT_POWERGATE_ENABLE_VDN_HCP_MASK(n)                        xe2_reg_bit(3 + 2 * (n))
#define XE2_REG_GT_POWERGATE_ENABLE_VDN_MFXVDENC_MASK(n)                   xe2_reg_bit(4 + 2 * (n))

#define XE2_REG_MTL_MEM_SS_INFO                                            0x45700 // Memory subsystem configuration
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_MASK                            xe2_reg_genmask(11, 8)
#define XE2_REG_MTL_MEM_SS_INFO_N_CHANNELS_MASK                            xe2_reg_genmask(7, 4)
#define XE2_REG_MTL_MEM_SS_INFO_DDR_TYPE_MASK                              xe2_reg_genmask(3, 0)

#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO(n)                           (0x45710 + (n) * 8)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_TRCD_MASK                    xe2_reg_genmask(31, 24)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_TRP_MASK                     xe2_reg_genmask(23, 16)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_LO_DCLK_MASK                    xe2_reg_genmask(15, 0)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI(n)                           (0x45710 + (n) * 8 + 4)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI_TRAS_MASK                    xe2_reg_genmask(16, 8)
#define XE2_REG_MTL_MEM_SS_INFO_QGV_POINTS_HI_TRDPRE_MASK                  xe2_reg_genmask(7, 0)

#define XE2_REG_STEER_SEMAPHORE                                            0xFD0
#define XE2_REG_GGC                                                        0x108040 // Graphics control
#define XE2_REG_GGC_GMS_MASK                                               xe2_reg_genmask(15, 8)
#define XE2_REG_GGC_GGMS_MASK                                              xe2_reg_genmask(7, 6)

#define XE2_REG_DSMBASE_LO                                                 0x1080C0
#define XE2_REG_DSMBASE_HI                                                 0x1080C4
#define XE2_REG_DSMBASE_64_MASK                                            xe2_reg_genmask64(63, 20)

#define XE2_REG_XEHP_TILE_ADDRESS_RANGE(n)                                 (0x4900 + (n) * 4)

#define XE2_REG_GSMBASE_LO                                                 0x108100
#define XE2_REG_GSMBASE_HI                                                 0x108104

#define XE2_REG_GUC_TLB_INV_DESC0                                          0xCF7C // Write-only for OS
#define XE2_REG_GUC_TLB_INV_DESC1                                          0xCF80 // Write-only for OS

#define XE2_REG_GU_CNTL_PROTECTED                                          0x10100C // This being used from i915
#define XE2_REG_GU_CNTL_PROTECTED_PRESENT_MASK                             xe2_reg_genmask(9, 9)

#define XE2_REG_VF_CAP                                                     0x1901F8
#define XE2_REG_VF_CAP_MASK                                                xe2_reg_genmask(0, 0)

#define XE2_REG_SDEISR                                                     0xC4000
#define XE2_REG_SDEIMR                                                     0xC4004
#define XE2_REG_SDEIIR                                                     0xC4008
#define XE2_REG_SDEIER                                                     0xC400C

#define XE2_REG_PSR_IMR_A                                                  0x60814
#define XE2_REG_PSR_IIR_A                                                  0x60818

#define XE2_REG_DE_PORT_ISR                                                0x44440
#define XE2_REG_DE_PORT_IMR                                                0x44444
#define XE2_REG_DE_PORT_IIR                                                0x44448
#define XE2_REG_DE_PORT_IER                                                0x4444c

#define XE2_REG_GU_MISC_ISR                                                0x444f0
#define XE2_REG_GU_MISC_IMR                                                0x444f4
#define XE2_REG_GU_MISC_IIR                                                0x444f8
#define XE2_REG_GU_MISC_IER                                                0x444fc

#define XE2_REG_DE_PIPE_ISR(pipe)                                          (0x44400 + (0x10 * (pipe)))
#define XE2_REG_DE_PIPE_IMR(pipe)                                          (0x44404 + (0x10 * (pipe)))
#define XE2_REG_DE_PIPE_IIR(pipe)                                          (0x44408 + (0x10 * (pipe)))
#define XE2_REG_DE_PIPE_IER(pipe)                                          (0x4440C + (0x10 * (pipe)))
// Bits within DE_PIPE_{ISR,IMR,IIR,IER}.
#define XE2_REG_DE_PIPE_VBLANK_MASK                                        xe2_reg_bit(0)
#define XE2_REG_DE_PIPE_FLIP_DONE_MASK                                     xe2_reg_bit(3)

#define XE2_PIPE_COUNT                                                     4

// Per-pipe frame counter. Pipe A = 0x70040, stride 0x1000 between pipes.
#define XE2_REG_PIPE_FRMCOUNT(pipe)                                        (0x70040 + (0x1000 * (pipe)))

// Top-level display interrupt control.
#define XE2_REG_GEN11_DISPLAY_INT_CTL                                      0x44200
#define XE2_REG_GEN11_DISPLAY_INT_CTL_ENABLE_MASK                          xe2_reg_bit(31)
#define XE2_REG_GEN11_DISPLAY_INT_CTL_PIPE_MASK(pipe)                      xe2_reg_bit(16 + (pipe))

#define XE2_PIPE_A                                                         0x0
#define XE2_PIPE_B                                                         0x1
#define XE2_PIPE_C                                                         0x2
#define XE2_PIPE_D                                                         0x3

#define XE2_REG_PLANE_WM_1_A_0                                             0x70240
#define XE2_REG_PLANE_WM_1_B_0                                             0x71240
#define XE2_REG_PLANE_WM_2_A_0                                             0x70340
#define XE2_REG_PLANE_WM_2_B_0                                             0x71340
#define XE2_REG_PLANE_WM_X_X_0_ENABLE_MASK                                 xe2_reg_bit(31)
#define XE2_REG_PLANE_WM_X_X_0_IGNORE_LINES_MASK                           xe2_reg_bit(30)
#define XE2_REG_PLANE_WM_X_X_0_AUTO_MIN_ALLOC_EN_MASK                      xe2_reg_bit(29)
#define XE2_REG_PLANE_WM_X_X_0_LINES_MASK                                  xe2_reg_bit(29)
#define XE2_REG_PLANE_WM_X_X_0_BLOCKS_MASK                                 xe2_reg_bit(29)

#define XE2_REG_DE_MISC_ISR                                                0x44460
#define XE2_REG_DE_MISC_IMR                                                0x44464
#define XE2_REG_DE_MISC_IIR                                                0x44468
#define XE2_REG_DE_MISC_IER                                                0x4446C

#define XE2_REG_DPA_AUX_CH_DATA(n)                                         (0x64014 + 4 * (n))
#define XE2_REG_DPB_AUX_CH_DATA(n)                                         (0x64114 + 4 * (n))
#define XE2_REG_DPX_AUX_CH_DATA_INDEX(reg)                                 ((reg - 0x64014) / 4)

#define XE2_REG_DPA_AUX_CH_CTL                                             0x64010
#define XE2_REG_DPB_AUX_CH_CTL                                             0x64110
#define XE2_REG_DPX_AUX_CH_CTL_SEND_BUSY_MASK                              xe2_reg_bit(31)
#define XE2_REG_DPX_AUX_CH_CTL_DONE_MASK                                   xe2_reg_bit(30)
#define XE2_REG_DPX_AUX_CH_CTL_INTERRUPT_MASK                              xe2_reg_bit(29)
#define XE2_REG_DPX_AUX_CH_CTL_TIME_OUT_ERROR_MASK                         xe2_reg_bit(28)
#define XE2_REG_DPX_AUX_CH_CTL_TIME_OUT_MASK                               xe2_reg_genmask(27, 26)
#define XE2_REG_DPX_AUX_CH_CTL_RECEIVE_ERROR_MASK                          xe2_reg_bit(25)
#define XE2_REG_DPX_AUX_CH_CTL_MSG_SIZE_MASK                               xe2_reg_genmask(24, 20)
#define XE2_REG_DPX_AUX_CH_CTL_POWER_REQUEST                               xe2_reg_bit(19)
#define XE2_REG_DPX_AUX_CH_CTL_POWER_STATUS                                xe2_reg_bit(18)
#define XE2_REG_DPX_AUX_CH_CTL_AUX_AKSV_SELECT_MASK                        xe2_reg_bit(15)
#define XE2_REG_DPX_AUX_CH_CTL_MANCHESTER_MASK                             xe2_reg_bit(14)
#define XE2_REG_DPX_AUX_CH_CTL_PSR_DATA_AUX_SKL_MASK                       xe2_reg_bit(14)
#define XE2_REG_DPX_AUX_CH_CTL_SYNC_TEST_MASK                              xe2_reg_bit(13)
#define XE2_REG_DPX_AUX_CH_CTL_FS_DATA_AUX_SKL_MASK                        xe2_reg_bit(13)
#define XE2_REG_DPX_AUX_CH_CTL_DEGLITCH_TEST__MASK                         xe2_reg_bit(12)
#define XE2_REG_DPX_AUX_CH_CTL_GTC_DATA_AUX_REG_MASK                       xe2_reg_bit(12)
#define XE2_REG_DPX_AUX_CH_CTL_PRECHARGE_TEST_MASK                         xe2_reg_bit(11)
#define XE2_REG_DPX_AUX_CH_CTL_TBT_IO_MASK                                 xe2_reg_bit(11)
#define XE2_REG_DPX_AUX_CH_CTL_BIT_CLOCK_2X_MASK                           xe2_reg_genmask(10, 0)
#define XE2_REG_DPX_AUX_CH_CTL_FW_SYNC_PULSE_SKL_MASK                      xe2_reg_genmask(9, 5)
#define XE2_REG_DPX_AUX_CH_CTL_SYNC_PUSLE_SKL_MASK                         xe2_reg_genmask(4, 0)

// Cx0 PHY per-lane message bus (PORT_A). The Cx0 PHYs are not directly
// addressable over MMIO: the host issues commands to a per-lane message
// bus and polls a status word for the PHY response.
#define XE2_REG_CX0_M2P_MSGBUS_CTL(lane)                                   (0x64040 + 4 * (lane))
#define XE2_REG_CX0_P2M_MSGBUS_STATUS(lane)                                (0x64048 + 4 * (lane))
#define XE2_CX0_LANE_TOTAL                                                 2

// Host -> PHY command word (M2P_MSGBUS_CTL).
#define XE2_REG_CX0_M2P_TRANSACTION_PENDING_MASK                           xe2_reg_bit(31)
#define XE2_REG_CX0_M2P_COMMAND_TYPE_MASK                                  xe2_reg_genmask(30, 27)
#define XE2_REG_CX0_M2P_DATA_MASK                                          xe2_reg_genmask(23, 16)
#define XE2_REG_CX0_M2P_TRANSACTION_RESET_MASK                             xe2_reg_bit(15)
#define XE2_REG_CX0_M2P_ADDRESS_MASK                                       xe2_reg_genmask(11, 0)

#define XE2_CX0_M2P_COMMAND_WRITE_UNCOMMITTED                              0x1
#define XE2_CX0_M2P_COMMAND_WRITE_COMMITTED                                0x2
#define XE2_CX0_M2P_COMMAND_READ                                           0x3

// PHY -> host status word (P2M_MSGBUS_STATUS).
#define XE2_REG_CX0_P2M_RESPONSE_READY_MASK                                xe2_reg_bit(31)
#define XE2_REG_CX0_P2M_COMMAND_TYPE_MASK                                  xe2_reg_genmask(30, 27)
#define XE2_REG_CX0_P2M_DATA_MASK                                          xe2_reg_genmask(23, 16)
#define XE2_REG_CX0_P2M_ERROR_SET_MASK                                     xe2_reg_bit(15)

#define XE2_CX0_P2M_COMMAND_READ_ACK                                       0x4
#define XE2_CX0_P2M_COMMAND_WRITE_ACK                                      0x5

// Indirect 16-bit SRAM access through special Cx0 register addresses. The
// host stages the address/data bytes, then a committed write to *_DATA_L
// performs the SRAM store; reads of *_DATA_H/L return the latched word.
#define XE2_CX0_REG_SRAM_WR_ADDRESS_H                                      0xC03
#define XE2_CX0_REG_SRAM_WR_ADDRESS_L                                      0xC02
#define XE2_CX0_REG_SRAM_WR_DATA_H                                         0xC05
#define XE2_CX0_REG_SRAM_WR_DATA_L                                         0xC04
#define XE2_CX0_REG_SRAM_RD_ADDRESS_H                                      0xC07
#define XE2_CX0_REG_SRAM_RD_ADDRESS_L                                      0xC06
#define XE2_CX0_REG_SRAM_RD_DATA_H                                         0xC09
#define XE2_CX0_REG_SRAM_RD_DATA_L                                         0xC08

// XELPDP_PORT_CLOCK_CTL (PORT_A). PLL/refclk request/ack handshakes, per lane.
#define XE2_REG_XELPDP_PORT_CLOCK_CTL                                      0x640E0
#define XE2_REG_XELPDP_PORT_CLOCK_CTL_PLL_REQUEST_MASK(lane)               xe2_reg_bit(31 - 4 * (lane))
#define XE2_REG_XELPDP_PORT_CLOCK_CTL_PLL_ACK_MASK(lane)                   xe2_reg_bit(30 - 4 * (lane))
#define XE2_REG_XELPDP_PORT_CLOCK_CTL_REFCLK_REQUEST_MASK(lane)            xe2_reg_bit(29 - 4 * (lane))
#define XE2_REG_XELPDP_PORT_CLOCK_CTL_REFCLK_ACK_MASK(lane)                xe2_reg_bit(28 - 4 * (lane))

// PORT_BUF_CTL1/2 (PORT_A). The PHY enable sequence polls these status bits;
// reflect or fix them so the sequence does not stall against zeroed registers.
#define XE2_REG_XELPDP_PORT_BUF_CTL1                                       0x64004
#define XE2_REG_XELPDP_PORT_BUF_CTL1_PHY_IDLE_MASK                         xe2_reg_bit(7)
#define XE2_REG_XELPDP_PORT_BUF_CTL1_SOC_PHY_READY_MASK                    xe2_reg_bit(24)
#define XE2_REG_XELPDP_PORT_BUF_CTL1_D2D_LINK_STATE_MASK                   xe2_reg_bit(28)
#define XE2_REG_XELPDP_PORT_BUF_CTL1_D2D_LINK_ENABLE_MASK                  xe2_reg_bit(29)
#define XE2_REG_XELPDP_PORT_BUF_CTL1_ENABLE                                xe2_reg_bit(31)
#define XE2_REG_XELPDP_PORT_BUF_CTL2                                       0x64008
#define XE2_REG_XELPDP_PORT_BUF_CTL2_LANE_PIPE_RESET_MASK                  xe2_reg_bit(31)
#define XE2_REG_XELPDP_PORT_BUF_CTL2_LANE_PHY_STATUS_MASK                  xe2_reg_bit(29)
#define XE2_REG_XELPDP_PORT_BUF_CTL2_POWERDOWN_UPDATE_MASK                 xe2_reg_genmask(25, 24)

// Transcoder A enable registers. The post-modeset readback derives pipe-active
// from these; they must read back enabled (TRANSCONF also exposes a state bit).
#define XE2_REG_TRANS_DDI_FUNC_CTL_A                                       0x60400
#define XE2_REG_TRANS_DDI_FUNC_CTL_ENABLE_MASK                             xe2_reg_bit(31)
#define XE2_REG_TRANSCONF_A                                                0x70008
#define XE2_REG_TRANSCONF_ENABLE_MASK                                      xe2_reg_bit(31)
#define XE2_REG_TRANSCONF_STATE_MASK                                       xe2_reg_bit(30)

#define XE2_REG_WM_LINETIME_A                                              0x45270
#define XE2_REG_WM_LINETIME_B                                              0x45274
#define XE2_REG_WM_LINETIME_X_HSW_LINETIME_MASK                            xe2_reg_genmask(8, 0)
#define XE2_REG_WM_LINETIME_X_HSW_IPS_LINETIME_MASK                        xe2_reg_genmask(24, 16)

#define XE2_REG_PP_STATUS                                                  0x61200 // Panel power sequence
#define XE2_REG_PP_ON_MASK                                                 xe2_reg_bit(31)
#define XE2_REG_PP_READY_MASK                                              xe2_reg_bit(30)
#define XE2_REG_PP_SEQUENCE_MASK                                           xe2_reg_genmask(29, 28)
#define XE2_REG_PP_CYCLE_DELAY_ACTIVE_MASK                                 xe2_reg_bit(27)

#define XE2_REG_PP_CONTROL                                                 0x61204
#define XE2_REG_PP_CONTROL_UNLOCK_MASK                                     xe2_reg_genmask(31, 16)
#define XE2_REG_PP_CONTROL_POWER_CYCLE_DELAY_MASK                          xe2_reg_genmask(8, 4)
#define XE2_REG_PP_CONTROL_EPD_FORCE_VDD_MASK                              xe2_reg_bit(3)
#define XE2_REG_PP_CONTROL_EPD_BLC_ENABLE_MASK                             xe2_reg_bit(2)
#define XE2_REG_PP_CONTROL_POWER_RESET_MASK                                xe2_reg_bit(1)
#define XE2_REG_PP_CONTROL_POWER_ON_MASK                                   xe2_reg_bit(0)

#define XE2_REG_PP_ON_DELAYS                                               0x61208
#define XE2_REG_PP_ON_DELAYS_PORT_SELECT_MASK                              xe2_reg_genmask(31, 30)
#define XE2_REG_PP_ON_DELAYS_POWER_ON_DELAY_MASK                           xe2_reg_genmask(28, 16)
#define XE2_REG_PP_ON_DELAYS_LIGHT_ON_DELAY_MASK                           xe2_reg_genmask(12, 0)

#define XE2_REG_PP_OFF_DELAYS                                              0x6120C
#define XE2_REG_PP_OFF_DELAYS_POWER_DOWN_DELAY_MASK                        xe2_reg_genmask(28, 16)
#define XE2_REG_PP_OFF_DELAYS_LIGHT_OFF_DELAY_MASK                         xe2_reg_genmask(12, 0)

#define XE2_REG_PP_DIVISOR                                                 0x61210
#define XE2_REG_PP_DIVISOR_REF_DIVIDER_MASK                                xe2_reg_genmask(31, 8)
#define XE2_REG_PP_DIVISOR_POWER_CYCLE_DELAY_MASK                          xe2_reg_genmask(4, 0)

#define XE2_REG_PCH_PP_STATUS                                              0xC7200
#define XE2_REG_PCH_PP_CONTROL                                             0xC7204
#define XE2_REG_PCH_PP_ON_DELAYS                                           0xC7208
#define XE2_REG_PCH_PP_OFF_DELAYS                                          0xC720C
#define XE2_REG_PCH_PP_DIVISOR                                             0xC7210

#define XE2_REG_RP_CONTROL                                                 0xA024
#define XE2_REG_RP_CONTROL_RPSWCTL_MASK                                    xe2_reg_genmask(10, 9)

#define XE2_REG_RC_CONTROL                                                 0xA090
#define XE2_REG_RC_CONTROL_CTL_HW_ENABLE_MASK                              xe2_reg_bit(31)
#define XE2_REG_RC_CONTROL_CTL_TO_MODE_MASK                                xe2_reg_bit(28)
#define XE2_REG_RC_CONTROL_CTL_RC6_ENABLE_MASK                             xe2_reg_bit(18)

#define XE2_REG_HSW_POWER_WELL_CTL1                                        0x45400
#define XE2_REG_HSW_POWER_WELL_CTL2                                        0x45404
#define XE2_REG_HSW_POWER_WELL_CTL3                                        0x45408
#define XE2_REG_HSW_POWER_WELL_CTL4                                        0x4540C

#define XE2_REG_BXT_DE_PLL_ENABLE                                          0x46070
#define XE2_REG_BXT_DE_PLL_ENABLE_MASK                                     xe2_reg_bit(31)
#define XE2_REG_BXT_DE_PLL_ENABLE_LOCK_MASK                                xe2_reg_bit(30)
#define XE2_REG_BXT_DE_PLL_ENABLE_FREQ_REQ_MASK                            xe2_reg_bit(23)
#define XE2_REG_BXT_DE_PLL_ENABLE_FREQ_REQ_ACK_MASK                        xe2_reg_bit(22)

#define XE2_REG_SKL_FUSE_STATUS                                            0x42000
#define XE2_REG_SKL_FUSE_STATUS_DOWNLOAD_MASK                              xe2_reg_bit(31)
// Power gates:
//   SKL_PG0 = 0
//   SKL_PG1 = 1
//   SKL_PG2 = 2
//   ICL_PG3 = 3
//   ICL_PG4 = 4
#define XE2_REG_SKL_FUSE_STATUS_DST_MASK(pg)                               (1 << (27 - (pg)))

#define XE2_REG_DBUF_CTL_S0                                                0x45008
#define XE2_REG_DBUF_CTL_S1                                                0x44FE8
#define XE2_REG_DBUF_CTL_S2                                                0x44300
#define XE2_REG_DBUF_CTL_S3                                                0x44304
#define XE2_REG_DBUF_CTL_SX_POWER_REQUEST_MASK                             xe2_reg_bit(31)
#define XE2_REG_DBUF_CTL_SX_POWER_STATE_MASK                               xe2_reg_bit(30)
#define XE2_REG_DBUF_CTL_SX_TRACKER_STATE_SERVICE_MASK                     xe2_reg_genmask(23, 19)
#define XE2_REG_DBUF_CTL_SX_MIN_TRACKER_STATE_SERVICE_MASK                 xe2_reg_genmask(18, 16)

#define XE2_REG_CDCLK_CTL                                                  0x46000 // Core display clock
#define XE2_REG_CDCLK_CTL_FREQ_SEL_MASK                                    xe2_reg_genmask(27, 26)
#define XE2_REG_CDCLK_CTL_SOURCE_SEL_MASK                                  xe2_reg_bit(25)
#define XE2_REG_CDCLK_CTL_CD2X_DIV_SEL_MASK                                xe2_reg_genmask(23, 22)
#define XE2_REG_CDCLK_CTL_CD2X_PIPE_MASK                                   xe2_reg_genmask(21, 20)

#define XE2_REG_SKL_DSSM                                                   0x51004 // Reference CDCLK
#define XE2_REG_SKL_DSSM_PLL_REFCLK_MASK                                   (7U << 29)
#define XE2_REG_SKL_DSSM_PLL_REFCLK_24MHZ                                  (0U << 29)
#define XE2_REG_SKL_DSSM_PLL_REFCLK_19_2MHZ                                (1U << 29)
#define XE2_REG_SKL_DSSM_PLL_REFCLK_38_4MHZ                                (2U << 29)

#define XE2_REG_DC_STATE_EN                                                0x45504
#define XE2_REG_DC_STATE_EN_DC3C0_MASK                                     xe2_reg_bit(30)
#define XE2_REG_DC_STATE_EN_DC3C0_STATUS_MASK                              xe2_reg_bit(29)
#define XE2_REG_DC_STATE_EN_HOLD_PHY_CLKREQ_PG1_LATCH_MASK                 xe2_reg_bit(21)
#define XE2_REG_DC_STATE_EN_HOLD_PHY_PG1_LATCH_MASK                        xe2_reg_bit(20)
#define XE2_REG_DC_STATE_EN_DC3C0                                          (1 << 30)
#define XE2_REG_DC_STATE_EN_UPTO_DC5                                       (1 << 0)
#define XE2_REG_DC_STATE_EN_UPTO_DC6                                       (2 << 0)
#define XE2_REG_DC_STATE_EN_DC9                                            (1 << 3)

#define XE2_REG_ILK_DPFC_CONTROL_1                                         0x43208
#define XE2_REG_ILK_DPFC_CONTROL_2                                         0x43248
#define XE2_REG_ILK_DPFC_CONTROL_X_EN_MASK                                 xe2_reg_bit(31)

#define XE2_REG_GUC_WOPCM_OFFSET                                           0xC340
#define XE2_REG_GUC_WOPCM_OFFSET_MASK                                      xe2_reg_genmask(31, 14)
#define XE2_REG_HUC_LOADING_AGENT_GUC_MASK                                 xe2_reg_bit(1)
#define XE2_REG_GUC_WOPCM_OFFSET_VALID                                     xe2_reg_bit(0)

// HuC kernel load/auth status. The driver triggers authentication via the GuC
// and polls this register for the success bit before marking HuC running.
#define XE2_REG_HUC_KERNEL_LOAD_INFO                                       0xC1DC
#define XE2_REG_HUC_KERNEL_LOAD_INFO_SUCCESSFUL                            xe2_reg_bit(0)

// WOPCM (Write once protected content memory)
// https://docs.kernel.org/gpu/xe/xe_firmware.html
#define XE2_REG_GUC_WOPCM_SIZE                                             0xC050
#define XE2_REG_GUC_WOPCM_SIZE_MASK                                        xe2_reg_genmask(31, 12)
#define XE2_REG_GUC_WOPCM_SIZE_LOCKED_MASK                                 xe2_reg_bit(0)

#define XE2_REG_GUC_PVC_TLB_INV_DESC0                                      0xCF7C
#define XE2_REG_GUC_PVC_TLB_INV_DESC1                                      0xCF80

#define XE2_REG_GUC_STATUS                                                 0xC000 // Probably RO
#define XE2_REG_GUC_STATUS_MASK                                            xe2_reg_genmask(31, 30)
#define XE2_REG_GUC_STATUS_MIA_MASK                                        xe2_reg_genmask(18, 16)
#define XE2_REG_GUC_STATUS_UKERNEL_MASK                                    xe2_reg_genmask(15, 8)
#define XE2_REG_GUC_STATUS_BOOTROM_MASK                                    xe2_reg_genmask(7, 1)
#define XE2_REG_GUC_STATUS_MIA_IN_RESET_MASK                               xe2_reg_bit(0)

#define XE2_REG_GUC_FW_SW_1                                                0x190240
#define XE2_REG_GUC_FW_SW_2                                                0x190244
#define XE2_REG_GUC_FW_SW_3                                                0x190248
#define XE2_REG_GUC_FW_SW_4                                                0x19024C
#define XE2_REG_GUC_FW_SW_X_MSG_0_ORIGIN_MASK                              xe2_reg_bit(31)
#define XE2_REG_GUC_FW_SW_X_MSG_0_TYPE_MASK                                xe2_reg_genmask(30, 28)
#define XE2_REG_GUC_FW_SW_X_MSG_0_DATA_MASK                                xe2_reg_genmask(27, 0)

#define XE2_REG_GUC_HOST_INTERRUPT                                         0x1901F0

#define XE2_REG_GUC_PMTIMESTAMP_LO                                         0xC3E8
#define XE2_REG_GUC_PMTIMESTAMP_HI                                         0xC3EC

#define XE2_REG_GUC_DMA_ADDR_0_LO                                          0xC300
#define XE2_REG_GUC_DMA_ADDR_0_HI                                          0xC304
#define XE2_REG_GUC_DMA_ADDR_1_LO                                          0xC308
#define XE2_REG_GUC_DMA_ADDR_1_HI                                          0xC30C
#define XE2_REG_GUC_DMA_COPY_SIZE                                          0xC310
#define XE2_REG_GUC_DMA_CTRL                                               0xC314
#define XE2_REG_GUC_DMA_CTRL_HUC_UKERNEL                                   xe2_reg_bit(9)
#define XE2_REG_GUC_DMA_CTRL_UOS_MOVE                                      xe2_reg_bit(4)
#define XE2_REG_GUC_DMA_CTRL_START_DMA                                     xe2_reg_bit(0)

#define XE2_REG_GUC_SOFT_SCRATCH(n)                                        (0xC180 + (n) * 4)
#define XE2_REG_GUC_SOFT_SCRATCH_INDEX(reg)                                ((reg - 0xC180) / 4)
#define XE2_GUC_SOFT_SCRATCH_COUNT                                         16

#define XE2_REG_STOLEN_RESERVED_LO                                         0x1082C0 // Das war schön gestohlen mal...
#define XE2_REG_STOLEN_RESERVED_HI                                         0x1082C4
#define XE2_REG_STOLEN_RESERVED_WOPCM_SIZE_MASK                            xe2_reg_genmask(9, 7)

#define XE2_DMC_FW_MAIN                                                    0
#define XE2_DMC_FW_PIPE_A                                                  1
#define XE2_DMC_FW_PIPE_B                                                  2
#define XE2_DMC_FW_PIPE_C                                                  3
#define XE2_DMC_FW_PIPE_D                                                  4
#define XE2_DMC_FW_PIPE_TOTAL                                              5

#define XE2_DMC_FW_MAIN_OFFSET                                             0x80000
#define XE2_DMC_FW_PIPE_A_OFFSET                                           0x90000
#define XE2_DMC_FW_PIPE_B_OFFSET                                           0x98000
#define XE2_DMC_FW_PIPE_C_OFFSET                                           0x52000
#define XE2_DMC_FW_PIPE_D_OFFSET                                           0x59000

#define XE2_REG_DMC_SSP_BASE                                               0x8F074

#define XE2_REG_XELP_GT_GEOMETRY_DSS_ENABLE                                0x913C
#define XE2_REG_XELP_XEHP_GT_COMPUTE_DSS_ENABLE                            0x9144
#define XE2_REG_XELP_EU_ENABLE                                             0x9134
#define XE2_REG_MIRROR_FUSE3                                               0x9118
#define XE2_REG_MIRROR_FUSE3_NODE_ENABLE_MASK                              xe2_reg_genmask(31, 16)
#define XE2_REG_MIRROR_FUSE3_XEHPC_GT_L3_MODE_MASK                         xe2_reg_genmask(7, 4)
#define XE2_REG_MIRROR_FUSE3_MEML3_EN_MASK                                 xe2_reg_genmask(3, 0)

#define XE2_REG_PLANE_CTL_1_A                                              0x70180 // We assume post-icl graphics
#define XE2_REG_PLANE_CTL_2_A                                              0x70280
#define XE2_REG_PLANE_CTL_1_B                                              0x71180
#define XE2_REG_PLANE_CTL_2_B                                              0x71280
#define XE2_REG_PLANE_CTL_X_ENABLE_MASK                                    xe2_reg_bit(31)
// Remaining plane-1 / pipe-A geometry registers (the plane fbcon scans out on).
#define XE2_REG_PLANE_STRIDE_1_A                                           0x70188 // Units of 64 bytes
#define XE2_REG_PLANE_SIZE_1_A                                             0x70190 // (h-1)<<16 | (w-1)
#define XE2_REG_PLANE_SURF_1_A                                             0x7019C // Surface GGTT byte offset
#define XE2_REG_PLANE_CTL_X_ICL_FORMAT_MASK                                xe2_reg_genmask(27, 23)
#define XE2_REG_PLANE_CTL_X_KEY_ENABLE_MASK                                xe2_reg_genmask(22, 21)
#define XE2_REG_PLANE_CTL_X_ORDER_RGBX_MASK                                xe2_reg_bit(20)
#define XE2_REG_PLANE_CTL_X_YUV420_Y_PLANE_MASK                            xe2_reg_bit(19)
#define XE2_REG_PLANE_CTL_X_YUV_TO_RGB_CSC_FORMAT_BT709_MASK               xe2_reg_bit(18)
#define XE2_REG_PLANE_CTL_X_YUV422_ORDER_MASK                              xe2_reg_genmask(17, 16)
#define XE2_REG_PLANE_CTL_X_RENDER_DECOMPRESSION_ENABLE_MASK               xe2_reg_bit(15)
#define XE2_REG_PLANE_CTL_X_TRICKLE_FEED_DISABLE_MASK                      xe2_reg_bit(14)
#define XE2_REG_PLANE_CTL_X_CLEAR_COLOR_DISABLE_MASK                       xe2_reg_bit(13)
#define XE2_REG_PLANE_CTL_X_TILED_MASK                                     xe2_reg_genmask(12, 10)
#define XE2_REG_PLANE_CTL_X_ASYNC_FLIP_MASK                                xe2_reg_bit(9)
#define XE2_REG_PLANE_CTL_X_FLIP_HORIZONTAL_MASK                           xe2_reg_bit(8)
#define XE2_REG_PLANE_CTL_X_MEDIA_DECOMPRESSION_ENABLE_MASK                xe2_reg_bit(4)
#define XE2_REG_PLANE_CTL_X_ROTATE_MASK                                    xe2_reg_genmask(1, 0)

// DSB - Display state buffer.
#define XE2_REG_DSB_BASE                                                   0x70B00
#define XE2_REG_DSB_INSTANCE(pipe, id)                                     (XE2_REG_DSB_BASE + (pipe) * 0x1000 + (id) * 0x100)
#define XE2_REG_DSB_HEAD(pipe, id)                                         (XE2_REG_DSB_INSTANCE(pipe, id) + 0x0)
#define XE2_REG_DSB_TAIL(pipe, id)                                         (XE2_REG_DSB_INSTANCE(pipe, id) + 0x4)
#define XE2_REG_DSB_CTRL(pipe, id)                                         (XE2_REG_DSB_INSTANCE(pipe, id) + 0x8)
#define XE2_REG_DSB_CTRL_ENABLE_MASK                                       xe2_reg_bit(31)
#define XE2_REG_DSB_CTRL_BUF_REITERATE_MASK                                xe2_reg_bit(29)
#define XE2_REG_DSB_CTRL_WAIT_FOR_VBLANK_MASK                              xe2_reg_bit(28)
#define XE2_REG_DSB_CTRL_WAIT_FOR_LINE_IN_MASK                             xe2_reg_bit(27)
#define XE2_REG_DSB_CTRL_HALT_MASK                                         xe2_reg_bit(16)
#define XE2_REG_DSB_CTRL_NON_POSTED_MASK                                   xe2_reg_bit(8)
#define XE2_REG_DSB_CTRL_STATUS_BUSY_MASK                                  xe2_reg_bit(0)
#define XE2_REG_DSB_MMIOCTRL(pipe, id)                                     (XE2_REG_DSB_INSTANCE(pipe, id) + 0xC)
#define XE2_REG_DSB_MMIOCTRL_DEAD_CLOCKS_ENABLE_MASK                       xe2_reg_bit(31)
#define XE2_REG_DSB_MMIOCTRL_DEAD_CLOCKS_COUNT_MASK                        xe2_reg_genmask(15, 8)
#define XE2_REG_DSB_MMIOCTRL_CYCLES_MASK                                   xe2_reg_genmask(7, 0)
#define XE2_REG_DSB_POLLFUNC(pipe, id)                                     (XE2_REG_DSB_INSTANCE(pipe, id) + 0x10)
#define XE2_REG_DSB_POLLFUNC_POLL_ENABLE_MASK                              xe2_reg_bit(31)
#define XE2_REG_DSB_POLLFUNC_WAIT_MASK                                     xe2_reg_genmask(30, 23)
#define XE2_REG_DSB_POLLFUNC_COUNT_MASK                                    xe2_reg_genmask(22, 15)
#define XE2_REG_DSB_DEBUG(pipe, id)                                        (XE2_REG_DSB_INSTANCE(pipe, id) + 0x14)
#define XE2_REG_DSB_POLLMASK(pipe, id)                                     (XE2_REG_DSB_INSTANCE(pipe, id) + 0x1C)
#define XE2_REG_DSB_STATUS(pipe, id)                                       (XE2_REG_DSB_INSTANCE(pipe, id) + 0x24)
#define XE2_REG_DSB_STATUS_HP_IDLE_STATUS_MASK                             xe2_reg_bit(31)
#define XE2_REG_DSB_STATUS_DEWAKE_STATUS_MASK                              xe2_reg_bit(30)
#define XE2_REG_DSB_STATUS_REQARB_SM_STATE_MASK                            xe2_reg_genmask(29, 27)
#define XE2_REG_DSB_STATUS_SAFE_WINDOW_LIVE_MASK                           xe2_reg_bit(26)
#define XE2_REG_DSB_STATUS_VTDFAULT_ARB_SM_STATE_MASK                      xe2_reg_genmask(25, 23)
#define XE2_REG_DSB_STATUS_TLBTRANS_SM_STATE_MASK                          xe2_reg_genmask(21, 20)
#define XE2_REG_DSB_STATUS_SAFE_WINDOW_MASK                                xe2_reg_bit(19)
#define XE2_REG_DSB_STATUS_POINTERS_SM_STATE_MASK                          xe2_reg_genmask(18, 17)
#define XE2_REG_DSB_STATUS_BUSY_DURING_DELAYED_VBLANK_MASK                 xe2_reg_bit(16)
#define XE2_REG_DSB_STATUS_MMIO_ARB_SM_STATE_MASK                          xe2_reg_genmask(15, 13)
#define XE2_REG_DSB_STATUS_MMIO_INST_SM_STATE_MASK                         xe2_reg_genmask(11, 7)
#define XE2_REG_DSB_STATUS_RESET_SM_STATE_MASK                             xe2_reg_genmask(5, 4)
#define XE2_REG_DSB_STATUS_RUN_SM_STATE_MASK                               xe2_reg_genmask(2, 0)
#define XE2_REG_DSB_INTERRUPT(pipe, id)                                    (XE2_REG_DSB_INSTANCE(pipe, id) + 0x28)
#define XE2_REG_DSB_INTERRUPT_GOSUB_INT_EN_MASK                            xe2_reg_bit(21)
#define XE2_REG_DSB_INTERRUPT_ATS_FAULT_INT_EN_MASK                        xe2_reg_bit(20)
#define XE2_REG_DSB_INTERRUPT_GTT_FAULT_INT_EN_MASK                        xe2_reg_bit(19)
#define XE2_REG_DSB_INTERRUPT_RSPTIMEOUT_INT_EN_MASK                       xe2_reg_bit(18)
#define XE2_REG_DSB_INTERRUPT_POLL_ERR_INT_EN_MASK                         xe2_reg_bit(17)
#define XE2_REG_DSB_INTERRUPT_PROG_INT_EN_MASK                             xe2_reg_bit(16)
#define XE2_REG_DSB_INTERRUPT_GOSUB_INT_STATUS_MASK                        xe2_reg_bit(5)
#define XE2_REG_DSB_INTERRUPT_ATS_FAULT_INT_STATUS_MASK                    xe2_reg_bit(4)
#define XE2_REG_DSB_INTERRUPT_GTT_FAULT_INT_STATUS_MASK                    xe2_reg_bit(3)
#define XE2_REG_DSB_INTERRUPT_RSPTIMEOUT_INT_STATUS_MASK                   xe2_reg_bit(2)
#define XE2_REG_DSB_INTERRUPT_POLL_ERR_INT_STATUS_MASK                     xe2_reg_bit(1)
#define XE2_REG_DSB_INTERRUPT_PROG_INT_STATUS_MASK                         xe2_reg_bit(0)
#define XE2_REG_DSB_CURRENT_HEAD(pipe, id)                                 (XE2_REG_DSB_INSTANCE(pipe, id) + 0x2C)
#define XE2_REG_DSB_RM_TIMEOUT(pipe, id)                                   (XE2_REG_DSB_INSTANCE(pipe, id) + 0x30)
#define XE2_REG_DSB_RM_TIMEOUT_CLAIM_TIMEOUT_MASK                          xe2_reg_bit(31)
#define XE2_REG_DSB_RM_TIMEOUT_READY_TIMEOUT_MASK                          xe2_reg_bit(30)
#define XE2_REG_DSB_RM_TIMEOUT_CLAIM_TIMEOUT_COUNT_MASK                    xe2_reg_genmask(23, 16)
#define XE2_REG_DSB_RM_TIMEOUT_READY_TIMEOUT_VALUE_MASK                    xe2_reg_genmask(15, 0)
#define XE2_REG_DSB_RMTIMEOUTREG_CAPTURE(pipe, id)                         (XE2_REG_DSB_INSTANCE(pipe, id) + 0x34)
#define XE2_REG_DSB_PMCTRL(pipe, id)                                       (XE2_REG_DSB_INSTANCE(pipe, id) + 0x38)
#define XE2_REG_DSB_PMCTRL_ENABLE_DEWAKE_MASK                              xe2_reg_bit(31)
#define XE2_REG_DSB_PMCTRL_SCANLINE_FOR_DEWAKE_MASK                        xe2_reg_genmask(30, 0)
#define XE2_REG_DSB_PMCTRL2(pipe, id)                                      (XE2_REG_DSB_INSTANCE(pipe, id) + 0x3C)
#define XE2_REG_DSB_PMCTRL2_MMIOGEN_DEWAKE_DIS_MASK                        xe2_reg_bit(31)
#define XE2_REG_DSB_PMCTRL2_FORCE_DEWAKE_MASK                              xe2_reg_bit(23)
#define XE2_REG_DSB_PMCTRL2_BLOCK_DEWAKE_EXTENSION_MASK                    xe2_reg_bit(15)
#define XE2_REG_DSB_PMCTRL2_OVERRIDE_DC5_DC6_OK_MASK                       xe2_reg_bit(7)
#define XE2_REG_DSB_PF_LN_LOWER(pipe, id)                                  (XE2_REG_DSB_INSTANCE(pipe, id) + 0x40)
#define XE2_REG_DSB_PF_LN_UPPER(pipe, id)                                  (XE2_REG_DSB_INSTANCE(pipe, id) + 0x44)
#define XE2_REG_DSB_BUFRPT_CNT(pipe, id)                                   (XE2_REG_DSB_INSTANCE(pipe, id) + 0x48)
#define XE2_REG_DSB_CHICKEN(pipe, id)                                      (XE2_REG_DSB_INSTANCE(pipe, id) + 0xF0)
#define XE2_REG_DSB_CHICKEN_FORCE_DMA_SYNC_RESET                           xe2_reg_bit(31)
#define XE2_REG_DSB_CHICKEN_FORCE_VTD_ENGIE_RESET                          xe2_reg_bit(30)
#define XE2_REG_DSB_CHICKEN_DISABLE_IPC_DEMOTE                             xe2_reg_bit(29)
#define XE2_REG_DSB_CHICKEN_SKIP_WAITS_EN                                  xe2_reg_bit(23)
#define XE2_REG_DSB_CHICKEN_EXTEND_HP_IDLE                                 xe2_reg_bit(16)
#define XE2_REG_DSB_CHICKEN_CTRL_WAIT_SAFE_WINDOW                          xe2_reg_bit(15)
#define XE2_REG_DSB_CHICKEN_CTRL_NO_WAIT_VBLANK                            xe2_reg_bit(14)
#define XE2_REG_DSB_CHICKEN_INST_WAIT_SAFE_WINDOW                          xe2_reg_bit(7)
#define XE2_REG_DSB_CHICKEN_INST_NO_WAIT_VBLANK                            xe2_reg_bit(6)
#define XE2_REG_DSB_CHICKEN_MMIOGEN_DEWAKE_DIS_CHICKEN                     xe2_reg_bit(2)
#define XE2_REG_DSB_CHICKEN_DISABLE_MMIO_COUNT_FOR_INDEXED                 xe2_reg_bit(0)

#define XE2_REG_DP_A                                                       0x64000
#define XE2_REG_DP_B                                                       0x64100
#define XE2_REG_DP_C                                                       0x64200
#define XE2_REG_DP_D                                                       0x64300
#define XE2_REG_DP_X_PORT_EN_MASK                                          xe2_reg_bit(31)

#define XE2_REG_TGL_DP_TP_STATUS_A                                         0x60544
#define XE2_REG_TGL_DP_TP_STATUS_FEC_ENABLE_LIVE                           xe2_reg_bit(28)
#define XE2_REG_TGL_DP_TP_STATUS_IDLE_DONE                                 xe2_reg_bit(25)
#define XE2_REG_TGL_DP_TP_STATUS_ACT_SENT                                  xe2_reg_bit(24)
#define XE2_REG_TGL_DP_TP_STATUS_MODE_STATUS_MST                           xe2_reg_bit(23)
#define XE2_REG_TGL_DP_TP_STATUS_STREAMS_ENABLED_MASK                      xe2_reg_genmask(18, 16) // 17:16 on hsw but bit 18 mbz
#define XE2_REG_TGL_DP_TP_STATUS_AUTOTRAIN_DONE                            xe2_reg_bit(12)
#define XE2_REG_TGL_DP_TP_STATUS_PAYLOAD_MAPPING_VC2_MASK                  xe2_reg_genmask(9, 8)
#define XE2_REG_TGL_DP_TP_STATUS_PAYLOAD_MAPPING_VC1_MASK                  xe2_reg_genmask(5, 4)
#define XE2_REG_TGL_DP_TP_STATUS_PAYLOAD_MAPPING_VC0_MASK                  xe2_reg_genmask(1, 0)

#define XE2_GUC_ACTION_DEFAULT                                             0x0
#define XE2_GUC_ACTION_REQUEST_PREEMPTION                                  0x2
#define XE2_GUC_ACTION_REQUEST_ENGINE_RESET                                0x3
#define XE2_GUC_ACTION_ALLOCATE_DOORBELL                                   0x10
#define XE2_GUC_ACTION_DEALLOCATE_DOORBELL                                 0x20
#define XE2_GUC_ACTION_LOG_BUFFER_FILE_FLUSH_COMPLETE                      0x30
#define XE2_GUC_ACTION_UK_LOG_ENABLE_LOGGING                               0x40
#define XE2_GUC_ACTION_FORCE_LOG_BUFFER_FLUSH                              0x302
#define XE2_GUC_ACTION_ENTER_S_STATE                                       0x501
#define XE2_GUC_ACTION_EXIT_S_STATE                                        0x502
#define XE2_GUC_ACTION_GLOBAL_SCHED_POLICY_CHANGE                          0x506
#define XE2_GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST                            0x3003
#define XE2_GUC_ACTION_HOST2GUC_SETUP_PC_GUCRC                             0x3004
#define XE2_GUC_ACTION_HOST2GUC_SELF_CFG                                   0x508
#define XE2_GUC_ACTION_UPDATE_SCHEDULING_POLICIES_KLV                      0x509
#define XE2_GUC_ACTION_SCHED_CONTEXT                                       0x1000
#define XE2_GUC_ACTION_SCHED_CONTEXT_MODE_SET                              0x1001
#define XE2_GUC_ACTION_SCHED_CONTEXT_MODE_DONE                             0x1002
#define XE2_GUC_ACTION_SCHED_ENGINE_MODE_SET                               0x1003
#define XE2_GUC_ACTION_SCHED_ENGINE_MODE_DONE                              0x1004
#define XE2_GUC_ACTION_SET_CONTEXT_PRIORITY                                0x1005
#define XE2_GUC_ACTION_SET_CONTEXT_EXECUTION_QUANTUM                       0x1006
#define XE2_GUC_ACTION_SET_CONTEXT_PREEMPTION_TIMEOUT                      0x1007
#define XE2_GUC_ACTION_CONTEXT_RESET_NOTIFICATION                          0x1008
#define XE2_GUC_ACTION_ENGINE_FAILURE_NOTIFICATION                         0x1009

#define XE2_GUC_ACTION_HOST2GUC_UPDATE_CONTEXT_POLICIES                    0x100B
#define XE2_UPDATE_CONTEXT_POLICIES_KLV_EXECUTION_QUANTUM                  0x2001
#define XE2_UPDATE_CONTEXT_POLICIES_KLV_PREEMPTION_TIMEOUT                 0x2002
#define XE2_UPDATE_CONTEXT_POLICIES_KLV_SCHEDULING_PRIORITY                0x2003
#define XE2_UPDATE_CONTEXT_POLICIES_KLV_PREEMPT_TO_IDLE_ON_QUANTUM_EXPIRY  0x2004
#define XE2_UPDATE_CONTEXT_POLICIES_KLV_SLPM_GT_FREQUENCY                  0x2005

#define XE2_GUC_ACTION_AUTHENTICATE_HUC                                    0x4000
#define XE2_GUC_ACTION_GET_HWCONFIG                                        0x4100
// Layout per 32-bit words:
// 00: Command
// 01: Flags
// 02: Context ID
// 03: Engine class
// 04: Engine submit mask
// 05: WQ desc lo
// 06: WQ desc hi
// 07: WQ base lo
// 08: WQ base hi
// 09: WQ size
// 10: HWLRCA lo
// 11: HWLRCA hi
#define XE2_GUC_ACTION_REGISTER_CONTEXT                                    0x4502

// hwconfig table attributes (key/length/value triplets). The driver reads the
// physical slice/subslice layout to derive register-steering targets; absent
// these keys it falls back to hardcoded values and logs an error. The counts
// describe the steering grid (not enabled DSS), and reproduce the per-group DSS
// count this Xe2 part otherwise assumes: ceil(subslices / slices) = 4.
#define XE2_HWCONFIG_ATTR_MAX_SLICES                                       1
#define XE2_HWCONFIG_ATTR_MAX_SUBSLICES                                    70
#define XE2_HWCONFIG_MAX_SLICES_VAL                                        4
#define XE2_HWCONFIG_MAX_SUBSLICES_VAL                                     16
#define XE2_GUC_ACTION_DEREGISTER_CONTEXT                                  0x4503
#define XE2_GUC_ACTION_REGISTER_COMMAND_TRANSPORT_BUFFER                   0x4505
#define XE2_GUC_ACTION_DEREGISTER_COMMAND_TRANSPORT_BUFFER                 0x4506
#define XE2_GUC_ACTION_REGISTER_G2G                                        0x4507
#define XE2_GUC_ACTION_DEREGISTER_G2G                                      0x4508
#define XE2_GUC_ACTION_HOST2GUC_CONTROL_CTB                                0x4509
#define XE2_GUC_ACTION_DEREGISTER_CONTEXT_DONE                             0x4600
#define XE2_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC                          0x4601
#define XE2_GUC_ACTION_CLIENT_SOFT_RESET                                   0x5507
#define XE2_GUC_ACTION_SET_ENG_UTIL_BUFF                                   0x550A
#define XE2_GUC_ACTION_SET_DEVICE_ENGINE_ACTIVITY_BUFFER                   0x550C
#define XE2_GUC_ACTION_SET_FUNCTION_ENGINE_ACTIVITY_BUFFER                 0x550D
#define XE2_GUC_ACTION_OPT_IN_FEATURE_KLV                                  0x550E
#define XE2_GUC_ACTION_NOTIFY_MEMORY_CAT_ERROR                             0x6000
#define XE2_GUC_ACTION_REPORT_PAGE_FAULT_REQ_DESC                          0x6002
#define XE2_GUC_ACTION_PAGE_FAULT_RES_DESC                                 0x6003
#define XE2_GUC_ACTION_ACCESS_COUNTER_NOTIFY                               0x6004
#define XE2_GUC_ACTION_TLB_INVALIDATION                                    0x7000
#define XE2_GUC_ACTION_TLB_INVALIDATION_DONE                               0x7001
#define XE2_GUC_ACTION_TLB_INVALIDATION_ALL                                0x7002
#define XE2_GUC_ACTION_STATE_CAPTURE_NOTIFICATION                          0x8002
#define XE2_GUC_ACTION_NOTIFY_FLUSH_LOG_BUFFER_TO_FILE                     0x8003
#define XE2_GUC_ACTION_NOTIFY_CRASH_DUMP_POSTED                            0x8004
#define XE2_GUC_ACTION_NOTIFY_EXCEPTION                                    0x8005
#define XE2_GUC_ACTION_TEST_G2G_SEND                                       0xF001
#define XE2_GUC_ACTION_TEST_G2G_RECV                                       0xF002

// SLPC (single-loop power controller / GT frequency) request sub-events. The
// SLPC request action 0x3003 carries an event id in msg[1] bits[31:8] and an
// argument count in bits[7:0]; the host publishes task state into a shared BO.
#define XE2_SLPC_EVENT_ID_MASK                                             xe2_reg_genmask(31, 8)
#define XE2_SLPC_EVENT_ARGC_MASK                                           xe2_reg_genmask(7, 0)
#define XE2_SLPC_EVENT_RESET                                               0
#define XE2_SLPC_EVENT_QUERY_TASK_STATE                                    5
#define XE2_SLPC_EVENT_PARAMETER_SET                                       6
#define XE2_SLPC_EVENT_PARAMETER_UNSET                                     7
#define XE2_SLPC_GLOBAL_STATE_RUNNING                                      3

// Shared-data byte offsets. The header occupies cacheline 0 (size @0,
// global_state @4); platform info fills cacheline 1; the task-state cacheline
// (status @0, freq @4) begins at cacheline 2.
#define XE2_SLPC_OFF_HEADER_SIZE                                           0x00
#define XE2_SLPC_OFF_GLOBAL_STATE                                          0x04
#define XE2_SLPC_OFF_TASK_STATE_FREQ                                       0x84
#define XE2_SLPC_SHARED_DATA_SIZE                                          0x2000

// task_state_data.freq sub-fields (raw ratios, 50/3 MHz per unit).
#define XE2_SLPC_FREQ_MAX_UNSLICE_MASK                                     xe2_reg_genmask(7, 0)
#define XE2_SLPC_FREQ_MIN_UNSLICE_MASK                                     xe2_reg_genmask(15, 8)

#define XE2_GUC_KLV_SELF_CFG_MEMIRQ_STATUS_ADDR_KEY                        0x900
#define XE2_GUC_KLV_SELF_CFG_MEMIRQ_STATUS_ADDR_LEN                        2
#define XE2_GUC_KLV_SELF_CFG_MEMIRQ_SOURCE_ADDR_KEY                        0x901
#define XE2_GUC_KLV_SELF_CFG_MEMIRQ_SOURCE_ADDR_LEN                        2
#define XE2_GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY                              0x902
#define XE2_GUC_KLV_SELF_CFG_H2G_CTB_ADDR_LEN                              2
#define XE2_GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY                   0x903
#define XE2_GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_LEN                   2
#define XE2_GUC_KLV_SELF_CFG_H2G_CTB_SIZE_KEY                              0x904
#define XE2_GUC_KLV_SELF_CFG_H2G_CTB_SIZE_LEN                              1
#define XE2_GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY                              0x905
#define XE2_GUC_KLV_SELF_CFG_G2H_CTB_ADDR_LEN                              2
#define XE2_GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY                   0x906
#define XE2_GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_LEN                   2
#define XE2_GUC_KLV_SELF_CFG_G2H_CTB_SIZE_KEY                              0x907
#define XE2_GUC_KLV_SELF_CFG_G2H_CTB_SIZE_LEN                              1

// GuC Command Transport Buffer (CTB) message framing. Each ring message is one
// CTB header dword followed by NUM_DWORDS payload dwords. The buffer descriptor
// holds head (consumer) and tail (producer) as dword indices into a circular
// command buffer.
#define XE2_GUC_CTB_HDR_LEN                                                1
#define XE2_GUC_CTB_MSG_0_FENCE                                            xe2_reg_genmask(31, 16)
#define XE2_GUC_CTB_MSG_0_FORMAT                                           xe2_reg_genmask(15, 12)
#define XE2_GUC_CTB_FORMAT_HXG                                             0
#define XE2_GUC_CTB_MSG_0_NUM_DWORDS                                       xe2_reg_genmask(7, 0)

// Host <-> GuC (HXG) message header, carried as the first payload dword.
#define XE2_GUC_HXG_MSG_0_ORIGIN                                           xe2_reg_bit(31)
#define XE2_GUC_HXG_ORIGIN_GUC                                             1
#define XE2_GUC_HXG_MSG_0_TYPE                                             xe2_reg_genmask(30, 28)
#define XE2_GUC_HXG_TYPE_REQUEST                                           0
#define XE2_GUC_HXG_TYPE_EVENT                                             1
#define XE2_GUC_HXG_TYPE_FAST_REQUEST                                      2
#define XE2_GUC_HXG_TYPE_RESPONSE_SUCCESS                                  7
#define XE2_GUC_HXG_MSG_0_ACTION                                           xe2_reg_genmask(15, 0)
#define XE2_GUC_HXG_RESPONSE_MSG_0_DATA0                                   xe2_reg_genmask(27, 0)

// Logical Ring Context (LRC) image layout. The register state follows the
// per-process HW status page (PPHWSP), one page in. Each entry below is a dword
// index into that register state; the value lives at (LRC base + 0x1000 + i*4).
#define XE2_LRC_REGS_OFFSET                                                0x1000
#define XE2_CTX_RING_HEAD                                                  0x5
#define XE2_CTX_RING_TAIL                                                  0x7
#define XE2_CTX_RING_START                                                 0x9
#define XE2_CTX_RING_CTL                                                   0xB
#define XE2_CTX_RING_TIMESTAMP                                             0x23
#define XE2_CTX_RING_TIMESTAMP_UDW                                         0x25
#define XE2_CTX_RING_PDP0_UDW                                              0x31
#define XE2_CTX_RING_PDP0_LDW                                              0x33
#define XE2_CTX_INT_STATUS_REPORT_PTR                                      0x57
#define XE2_CTX_INT_SRC_REPORT_PTR                                         0x59
#define XE2_CTX_CS_INT_VEC_DATA                                            0x5B

// Memory-based interrupts (memirq). The engine's source-report and
// status-report GGTT pointers are baked into its LRC register state (the driver
// never sends them over self-cfg). The render engine sits at irq_offset 0, and
// its user-interrupt is byte 0 of the status vector; each cause is a byte 0xFF.
#define XE2_MEMIRQ_RENDER_SRC_BYTE                                         0
#define XE2_MEMIRQ_RENDER_STATUS_BYTE                                      0
#define XE2_MEMIRQ_BYTE_SET                                                0xFF
#define XE2_LRC_SEQNO_PPHWSP_OFFSET                                        512
#define XE2_LRC_START_SEQNO_PPHWSP_OFFSET                                  520
#define XE2_LRC_CTX_JOB_TIMESTAMP_OFFSET                                   528
#define XE2_LRC_PARALLEL_OFFSET                                            2048

// memirq page geometry. The source-report page sits 0x400 into the shared BO;
// each status vector is 16 bytes. The GuC's own interrupt sits at bit 25
// (INTR_GUC): source byte at +0x400+25, status vector at +25*16, and its
// GuC-to-host cause is byte 15 (GUC_INTR_GUC2HOST) of that vector.
#define XE2_MEMIRQ_SOURCE_PAGE_OFFSET                                      0x400
#define XE2_MEMIRQ_VECTOR_STRIDE                                           16
#define XE2_MEMIRQ_INTR_GUC                                                25
#define XE2_MEMIRQ_GUC2HOST_BYTE                                           15

// Register-based top-level interrupt chain (native, non-VF). MSI-X vector 0 is
// wired to this handler; it consults these registers to locate the source. The
// GuC sits in GT_INTR_DW bank 0 at INTR_GUC (bit 25), reported as engine class
// OTHER (4) instance GUC (0) in the identity register.
#define XE2_REG_DG1_MSTR_TILE_INTR                                         0x190008
#define XE2_REG_GFX_MSTR_IRQ                                               0x190010
#define XE2_REG_GT_INTR_DW0                                                0x190018
#define XE2_REG_GT_INTR_DW1                                                0x19001C
#define XE2_REG_INTR_IDENTITY_REG0                                         0x190060
#define XE2_REG_INTR_IDENTITY_REG1                                         0x190064
#define XE2_REG_IIR_REG_SELECTOR0                                          0x190070
#define XE2_REG_IIR_REG_SELECTOR1                                          0x190074
#define XE2_IRQ_MASTER_BIT                                                 (1U << 31)
#define XE2_IRQ_DG1_TILE0_BIT                                              (1U << 0)
#define XE2_IRQ_GT_DW0_BIT                                                 (1U << 0)
#define XE2_IRQ_DISPLAY_BIT                                                (1U << 16) // GFX_MSTR_IRQ display source
#define XE2_IRQ_INTR_GUC_BIT                                               (1U << 25)
#define XE2_IRQ_INTR_DATA_VALID                                            (1U << 31)
#define XE2_IRQ_ENGINE_CLASS_OTHER                                         4
#define XE2_IRQ_GUC_INSTANCE                                               0
// GuC interrupt cause carried in the identity register's intr_vec (bits 15:0);
// the GuC handler queues the G2H worker only when GUC_INTR_GUC2HOST is set.
#define XE2_IRQ_GUC2HOST_VEC                                               (1U << 15)

#define XE2_HW_ENGINE_RENDER_RING_BASE                                     0x02000
#define XE2_HW_ENGINE_BSD_RING_BASE                                        0x1C0000
#define XE2_HW_ENGINE_BSD2_RING_BASE                                       0x1C4000
#define XE2_HW_ENGINE_BSD3_RING_BASE                                       0x1D0000
#define XE2_HW_ENGINE_BSD4_RING_BASE                                       0x1D4000
#define XE2_HW_ENGINE_XEHP_BSD5_RING_BASE                                  0x1E0000
#define XE2_HW_ENGINE_XEHP_BSD6_RING_BASE                                  0x1E4000
#define XE2_HW_ENGINE_XEHP_BSD7_RING_BASE                                  0x1F0000
#define XE2_HW_ENGINE_XEHP_BSD8_RING_BASE                                  0x1F4000
#define XE2_HW_ENGINE_VEBOX_RING_BASE                                      0x1C8000
#define XE2_HW_ENGINE_VEBOX2_RING_BASE                                     0x1D8000
#define XE2_HW_ENGINE_XEHP_VEBOX3_RING_BASE                                0x1E8000
#define XE2_HW_ENGINE_XEHP_VEBOX4_RING_BASE                                0x1F8000
#define XE2_HW_ENGINE_COMPUTE0_RING_BASE                                   0x1A000
#define XE2_HW_ENGINE_COMPUTE1_RING_BASE                                   0x1C000
#define XE2_HW_ENGINE_COMPUTE2_RING_BASE                                   0x1E000
#define XE2_HW_ENGINE_COMPUTE3_RING_BASE                                   0x26000
#define XE2_HW_ENGINE_BLT_RING_BASE                                        0x22000
#define XE2_HW_ENGINE_XEHPC_BCS1_RING_BASE                                 0x3E0000
#define XE2_HW_ENGINE_XEHPC_BCS2_RING_BASE                                 0x3E2000
#define XE2_HW_ENGINE_XEHPC_BCS3_RING_BASE                                 0x3E4000
#define XE2_HW_ENGINE_XEHPC_BCS4_RING_BASE                                 0x3E6000
#define XE2_HW_ENGINE_XEHPC_BCS5_RING_BASE                                 0x3E8000
#define XE2_HW_ENGINE_XEHPC_BCS6_RING_BASE                                 0x3EA000
#define XE2_HW_ENGINE_XEHPC_BCS7_RING_BASE                                 0x3EC000
#define XE2_HW_ENGINE_XEHPC_BCS8_RING_BASE                                 0x3EE000
#define XE2_HW_ENGINE_GSCCS_RING_BASE                                      0x11A000

#define XE2_REG_HW_ENGINE_CLASS(base)                                      (base + 0x8C)
#define XE2_REG_HW_ENGINE_CLASS_INSTANCE_ID_MASK                           xe2_reg_genmask(9, 4)
#define XE2_REG_HW_ENGINE_CLASS_ID_MASK                                    xe2_reg_genmask(2, 0)

#define XE2_REG_HW_ENGINE_RING_IDLEDLY(base)                               (base + 0x23C)
#define XE2_REG_HW_ENGINE_RING_IDLEDLY_INHIBIT_SWITCH_UNTIL_PREEMPTED_MASK xe2_reg_bit(31)
#define XE2_REG_HW_ENGINE_RING_IDLEDLY_IDLE_DELAY_MASK                     xe2_reg_genmask(20, 0)

#define XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT(base)                         (base + 0x54)
#define XE2_REG_HW_ENGINE_RING_PWRCTX_MAXCNT_IDLE_WAIT_TIME_MASK           xe2_reg_genmask(19, 0)

#define XE2_REG_HW_ENGINE_RING_MI_MODE(base)                               (base + 0x9C)

#define XE2_REG_HW_ENGINE_RING_TAIL(base)                                  (base + 0x30)
#define XE2_REG_HW_ENGINE_RING_HEAD(base)                                  (base + 0x34)
#define XE2_REG_HW_ENGINE_RING_START(base)                                 (base + 0x38)
#define XE2_REG_HW_ENGINE_RING_CTL(base)                                   (base + 0x3C)
#define XE2_REG_HW_ENGINE_RING_TIMESTAMP(base)                             (base + 0x358)
#define XE2_REG_HW_ENGINE_RING_CTX_TIMESTAMP(base)                         (base + 0x3A8)

#define XE2_VRAM_SIZE                                                      0x10000000 // 256 MiB

// https://lists.freedesktop.org/archives/intel-xe/2023-June/005371.html
#define XE2_GGTT_PTE_VALID                                                 (1ULL << 0)
#define XE2_GGTT_PAGES                                                     0x100000
#define XE2_GGTT_PTE_ADDR_MASK                                             0x0000FFFFFFFFF000ULL
#define XE2_GGTT_MMIO_BASE                                                 0x800000 // 8 MiB
#define XE2_GGTT_MMIO_SIZE                                                 0x800000 // 8 MiB

// Theoretically this number of contexts (rcs0/bcs0/ccs0/...) may be registered at once.
// This is extremely pessimistic scenario, just compatible with original Xe2 driver.
#define XE2_MAX_CONTEXTS                                                   65535

// Ring command stream decode. Type lives in bits 31:29 (MI = 0, GFXPIPE = 3);
// the MI opcode is bits 28:23. The completion postamble a job appends to its
// ring stores the seqno (MI_STORE_DATA_IMM / MI_FLUSH_DW / PIPE_CONTROL with a
// GGTT post-sync write) and raises MI_USER_INTERRUPT.
#define XE2_INSTR_TYPE(h)                                                  ((h) >> 29)
#define XE2_INSTR_TYPE_MI                                                  0
#define XE2_INSTR_TYPE_GSC                                                 2
#define XE2_INSTR_TYPE_GFXPIPE                                             3
#define XE2_INSTR_TYPE_GFX_STATE                                           4
// Undocumented in kernel & Mesa command type. As per
// $ https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/intel/genxml/xe2.xml?ref_type=heads
// this command type use only one kind of instruction.
//
// <instruction name="RESOURCE_BARRIER" bias="2" length="5" engine="render|compute">
//   <field name="DWord Length" dword="0" bits="7:0" type="uint" default="3" />
//   <field name="Predicate Enable" dword="0" bits="24:24" type="bool" />
//   <field name="Opcode" dword="0" bits="28:26" type="uint" default="3">
//     <value name="RESOURCE_BARRIER" value="3" />
//   </field>
//   <field name="Command Type" dword="0" bits="31:29" type="uint" default="5" />
//   <field name="Resource Barrier Body" dword="1" bits="127:0" type="RESOURCE_BARRIER_BODY" />
// </instruction>
#define XE2_INSTR_TYPE_RESOURCE_BARRIER                                    5
#define XE2_MI_OPCODE(h)                                                   (((h) >> 23) & 0x3F) // [28:23]
#define XE2_MI_OP_NOOP                                                     0x00
#define XE2_MI_OP_USER_INTERRUPT                                           0x02
#define XE2_MI_OP_ARB_CHECK                                                0x05
#define XE2_MI_OP_ARB_ON_OFF                                               0x08
#define XE2_MI_OP_BATCH_BUFFER_END                                         0x0a
#define XE2_MI_OP_STORE_DATA_IMM                                           0x20
#define XE2_MI_OP_STORE_REG_MEM                                            0x24
#define XE2_MI_SRM_USE_GGTT                                                xe2_reg_bit(22)
#define XE2_MI_SRM_ADD_CS_OFFSET                                           xe2_reg_bit(19)
#define XE2_MI_OP_FLUSH_DW                                                 0x26
#define XE2_MI_OP_BATCH_BUFFER_START                                       0x31
#define XE2_MI_OP_BATCH_BUFFER_START_PPGTT                                 xe2_reg_bit(8)
#define XE2_MI_SDI_GGTT                                                    xe2_reg_bit(22)
#define XE2_MI_FLUSH_DW_OP_STOREDW                                         xe2_reg_bit(14)
#define XE2_MI_FLUSH_DW_USE_GTT                                            xe2_reg_bit(2)

// GFXPIPE (Graphics pipe) commands are mainly used to configure GPU
// pipeline. Actual rendering logic is contained in shaders submitted
// with 3DSTATE_VS.
//
// We ignore pipeline field for simplicity now. Probably there is no
// command which indicies could clatch with other instruction.
#define XE2_GFXPIPE_PIPELINE(h)                                            (((h) >> 27) & 0x3)  // [28:27]
#define XE2_GFXPIPE_OPCODE(h)                                              (((h) >> 24) & 0x7)  // [26:24]
#define XE2_GFXPIPE_SUBOPCODE(h)                                           (((h) >> 16) & 0xFF) // [23:16]
#define XE2_GFXPIPE_PIPELINE_COMMON                                        0x00
#define XE2_GFXPIPE_PIPELINE_SINGLE_DW                                     0x01
#define XE2_GFXPIPE_PIPELINE_COMPUTE                                       0x02
#define XE2_GFXPIPE_PIPELINE_3D                                            0x03
// Mask [28:16] dedicated for pipeline + opcode + subopcode triplet.
#define XE2_GFXPIPE_OPCODES_MASKED(h)                                      (h & 0x1FFF0000)
// [28:27] - 3D command pipeline (common/single DW/compute/3D).
// [26:24] - 3D command opcode.
// [23:16] - 3D command subopcode.
#define XE2_GFXPIPE_CMD(pipeline, op, subop)                               ((pipeline & 0x3) << 27 | (op & 0x7) << 24 | (subop & 0xFF) << 16)
#define XE2_GFXPIPE_CMD_3D(op, subop)                                      XE2_GFXPIPE_CMD(XE2_GFXPIPE_PIPELINE_3D, op, subop)
#define XE2_GFXPIPE_CMD_COMMON(op, subop)                                  XE2_GFXPIPE_CMD(XE2_GFXPIPE_PIPELINE_COMMON, op, subop)
#define XE2_GFXPIPE_CMD_STATE_BASE_ADDRESS                                 XE2_GFXPIPE_CMD_COMMON(0x1, 0x1)
#define XE2_GFXPIPE_CMD_STATE_SIP                                          XE2_GFXPIPE_CMD_COMMON(0x1, 0x2)
#define XE2_GFXPIPE_CMD_GPGPU_CSR_BASE_ADDRESS                             XE2_GFXPIPE_CMD_COMMON(0x1, 0x4)
#define XE2_GFXPIPE_CMD_STATE_COMPUTE_MODE                                 XE2_GFXPIPE_CMD_COMMON(0x1, 0x5)
#define XE2_GFXPIPE_CMD_3DSTATE_BTD                                        XE2_GFXPIPE_CMD_COMMON(0x1, 0x6)
#define XE2_GFXPIPE_CMD_STATE_SYSTEM_MEM_FENCE_ADDRESS                     XE2_GFXPIPE_CMD_COMMON(0x1, 0x9)
#define XE2_GFXPIPE_CMD_STATE_CONTEXT_DATA_BASE_ADDRESS                    XE2_GFXPIPE_CMD_COMMON(0x1, 0xB)
#define XE2_GFXPIPE_CMD_PIPE_CONTROL                                       XE2_GFXPIPE_CMD_3D(0x2, 0x0)
#define XE2_GFXPIPE_CMD_PIPE_CONTROL_QW_WRITE                              xe2_reg_bit(14)
#define XE2_GFXPIPE_CMD_PIPE_CONTROL_GLOBAL_GTT                            xe2_reg_bit(24)
#define XE2_GFXPIPE_CMD_3DSTATE_DRAWING_RECTANGLE_FAST                     XE2_GFXPIPE_CMD_3D(0x0, 0x00)
#define XE2_GFXPIPE_CMD_3DSTATE_CLEAR_PARAMS                               XE2_GFXPIPE_CMD_3D(0x0, 0x04)
#define XE2_GFXPIPE_CMD_3DSTATE_DEPTH_BUFFER                               XE2_GFXPIPE_CMD_3D(0x0, 0x05)
#define XE2_GFXPIPE_CMD_3DSTATE_STENCIL_BUFFER                             XE2_GFXPIPE_CMD_3D(0x0, 0x06)
#define XE2_GFXPIPE_CMD_3DSTATE_HIER_DEPTH_BUFFER                          XE2_GFXPIPE_CMD_3D(0x0, 0x07)
#define XE2_GFXPIPE_CMD_3DSTATE_VERTEX_BUFFERS                             XE2_GFXPIPE_CMD_3D(0x0, 0x08)
#define XE2_GFXPIPE_CMD_3DSTATE_VERTEX_ELEMENTS                            XE2_GFXPIPE_CMD_3D(0x0, 0x09)
#define XE2_GFXPIPE_CMD_3DSTATE_INDEX_BUFFER                               XE2_GFXPIPE_CMD_3D(0x0, 0x0A)
#define XE2_GFXPIPE_CMD_3DSTATE_VF                                         XE2_GFXPIPE_CMD_3D(0x0, 0x0C)
#define XE2_GFXPIPE_CMD_3DSTATE_MULTISAMPLE                                XE2_GFXPIPE_CMD_3D(0x0, 0x0D)
#define XE2_GFXPIPE_CMD_3DSTATE_CC_STATE_POINTERS                          XE2_GFXPIPE_CMD_3D(0x0, 0x0E)
#define XE2_GFXPIPE_CMD_3DSTATE_SCISSOR_STATE_POINTERS                     XE2_GFXPIPE_CMD_3D(0x0, 0x0F)
#define XE2_GFXPIPE_CMD_3DSTATE_VS                                         XE2_GFXPIPE_CMD_3D(0x0, 0x10) // Vertex shader
#define XE2_GFXPIPE_CMD_3DSTATE_GS                                         XE2_GFXPIPE_CMD_3D(0x0, 0x11) // Geometry shader
#define XE2_GFXPIPE_CMD_3DSTATE_CLIP                                       XE2_GFXPIPE_CMD_3D(0x0, 0x12)
#define XE2_GFXPIPE_CMD_3DSTATE_SF                                         XE2_GFXPIPE_CMD_3D(0x0, 0x13)
#define XE2_GFXPIPE_CMD_3DSTATE_WM                                         XE2_GFXPIPE_CMD_3D(0x0, 0x14)
#define XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_VS                                XE2_GFXPIPE_CMD_3D(0x0, 0x15)
#define XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_GS                                XE2_GFXPIPE_CMD_3D(0x0, 0x16)
#define XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_PS                                XE2_GFXPIPE_CMD_3D(0x0, 0x17)
#define XE2_GFXPIPE_CMD_3DSTATE_SAMPLE_MASK                                XE2_GFXPIPE_CMD_3D(0x0, 0x18)
#define XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_HS                                XE2_GFXPIPE_CMD_3D(0x0, 0x19)
#define XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_DS                                XE2_GFXPIPE_CMD_3D(0x0, 0x1A)
#define XE2_GFXPIPE_CMD_3DSTATE_HS                                         XE2_GFXPIPE_CMD_3D(0x0, 0x1B) // Hull shader
#define XE2_GFXPIPE_CMD_3DSTATE_TE                                         XE2_GFXPIPE_CMD_3D(0x0, 0x1C)
#define XE2_GFXPIPE_CMD_3DSTATE_DS                                         XE2_GFXPIPE_CMD_3D(0x0, 0x1D) // Domain shader
#define XE2_GFXPIPE_CMD_3DSTATE_STREAMOUT                                  XE2_GFXPIPE_CMD_3D(0x0, 0x1E)
#define XE2_GFXPIPE_CMD_3DSTATE_SBE                                        XE2_GFXPIPE_CMD_3D(0x0, 0x1F)
#define XE2_GFXPIPE_CMD_3DSTATE_PS                                         XE2_GFXPIPE_CMD_3D(0x0, 0x20) // Pixel shader
#define XE2_GFXPIPE_CMD_3DSTATE_VIEWPORT_STATE_PTR_SF_CLIP                 XE2_GFXPIPE_CMD_3D(0x0, 0x21)
#define XE2_GFXPIPE_CMD_3DSTATE_CPS_POINTERS                               XE2_GFXPIPE_CMD_3D(0x0, 0x22)
#define XE2_GFXPIPE_CMD_3DSTATE_VIEWPORT_STATE_POINTERS_CC                 XE2_GFXPIPE_CMD_3D(0x0, 0x23)
#define XE2_GFXPIPE_CMD_3DSTATE_BLEND_STATE_POINTERS                       XE2_GFXPIPE_CMD_3D(0x0, 0x24)
#define XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_VS                  XE2_GFXPIPE_CMD_3D(0x0, 0x26)
#define XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_HS                  XE2_GFXPIPE_CMD_3D(0x0, 0x27)
#define XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_DS                  XE2_GFXPIPE_CMD_3D(0x0, 0x28)
#define XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_GS                  XE2_GFXPIPE_CMD_3D(0x0, 0x29)
#define XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_PS                  XE2_GFXPIPE_CMD_3D(0x0, 0x2A)
#define XE2_GFXPIPE_CMD_3DSTATE_SAMPLER_STATE_POINTERS_VS                  XE2_GFXPIPE_CMD_3D(0x0, 0x2B)
#define XE2_GFXPIPE_CMD_3DSTATE_SAMPLER_STATE_POINTERS_HS                  XE2_GFXPIPE_CMD_3D(0x0, 0x2C)
#define XE2_GFXPIPE_CMD_3DSTATE_SAMPLER_STATE_POINTERS_DS                  XE2_GFXPIPE_CMD_3D(0x0, 0x2D)
#define XE2_GFXPIPE_CMD_3DSTATE_SAMPLER_STATE_POINTERS_GS                  XE2_GFXPIPE_CMD_3D(0x0, 0x2E)
#define XE2_GFXPIPE_CMD_3DSTATE_SAMPLER_STATE_POINTERS_PS                  XE2_GFXPIPE_CMD_3D(0x0, 0x2F)
#define XE2_GFXPIPE_CMD_3DSTATE_VF_INSTANCING                              XE2_GFXPIPE_CMD_3D(0x0, 0x49)
#define XE2_GFXPIPE_CMD_3DSTATE_VF_SGVS                                    XE2_GFXPIPE_CMD_3D(0x0, 0x4A)
#define XE2_GFXPIPE_CMD_3DSTATE_VF_TOPOLOGY                                XE2_GFXPIPE_CMD_3D(0x0, 0x4B)
#define XE2_GFXPIPE_CMD_3DSTATE_WM_CHROMAKEY                               XE2_GFXPIPE_CMD_3D(0x0, 0x4C)
#define XE2_GFXPIPE_CMD_3DSTATE_PS_BLEND                                   XE2_GFXPIPE_CMD_3D(0x0, 0x4D)
#define XE2_GFXPIPE_CMD_3DSTATE_WM_DEPTH_STENCIL                           XE2_GFXPIPE_CMD_3D(0x0, 0x4E)
#define XE2_GFXPIPE_CMD_3DSTATE_PS_EXTRA                                   XE2_GFXPIPE_CMD_3D(0x0, 0x4F)
#define XE2_GFXPIPE_CMD_3DSTATE_RASTER                                     XE2_GFXPIPE_CMD_3D(0x0, 0x50)
#define XE2_GFXPIPE_CMD_3DSTATE_SBE_SWIZ                                   XE2_GFXPIPE_CMD_3D(0x0, 0x51)
#define XE2_GFXPIPE_CMD_3DSTATE_WM_HZ_OP                                   XE2_GFXPIPE_CMD_3D(0x0, 0x52)
#define XE2_GFXPIPE_CMD_3DSTATE_VF_COMPONENT_PACKING                       XE2_GFXPIPE_CMD_3D(0x0, 0x55)
#define XE2_GFXPIPE_CMD_3DSTATE_VF_SGVS_2                                  XE2_GFXPIPE_CMD_3D(0x0, 0x56)
#define XE2_GFXPIPE_CMD_3DSTATE_VFG                                        XE2_GFXPIPE_CMD_3D(0x0, 0x57)
#define XE2_GFXPIPE_CMD_3DSTATE_URB_ALLOC_VS                               XE2_GFXPIPE_CMD_3D(0x0, 0x58)
#define XE2_GFXPIPE_CMD_3DSTATE_URB_ALLOC_HS                               XE2_GFXPIPE_CMD_3D(0x0, 0x59)
#define XE2_GFXPIPE_CMD_3DSTATE_URB_ALLOC_DS                               XE2_GFXPIPE_CMD_3D(0x0, 0x5A)
#define XE2_GFXPIPE_CMD_3DSTATE_URB_ALLOC_GS                               XE2_GFXPIPE_CMD_3D(0x0, 0x5B)
#define XE2_GFXPIPE_CMD_3DSTATE_SO_BUFFER_INDEX_0                          XE2_GFXPIPE_CMD_3D(0x0, 0x60)
#define XE2_GFXPIPE_CMD_3DSTATE_SO_BUFFER_INDEX_1                          XE2_GFXPIPE_CMD_3D(0x0, 0x61)
#define XE2_GFXPIPE_CMD_3DSTATE_SO_BUFFER_INDEX_2                          XE2_GFXPIPE_CMD_3D(0x0, 0x62)
#define XE2_GFXPIPE_CMD_3DSTATE_SO_BUFFER_INDEX_3                          XE2_GFXPIPE_CMD_3D(0x0, 0x63)
#define XE2_GFXPIPE_CMD_3DSTATE_PRIMITIVE_REPLICATION                      XE2_GFXPIPE_CMD_3D(0x0, 0x6C)
#define XE2_GFXPIPE_CMD_3DSTATE_TBIMR_TILE_PASS_INFO                       XE2_GFXPIPE_CMD_3D(0x0, 0x6E)
#define XE2_GFXPIPE_CMD_3DSTATE_AMFS                                       XE2_GFXPIPE_CMD_3D(0x0, 0x6F)
#define XE2_GFXPIPE_CMD_3DSTATE_DEPTH_BOUNDS                               XE2_GFXPIPE_CMD_3D(0x0, 0x71)
#define XE2_GFXPIPE_CMD_3DSTATE_AMFS_TEXTURE_POINTERS                      XE2_GFXPIPE_CMD_3D(0x0, 0x72)
#define XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_TS_POINTER                        XE2_GFXPIPE_CMD_3D(0x0, 0x73)
#define XE2_GFXPIPE_CMD_3DSTATE_MESH_CONTROL                               XE2_GFXPIPE_CMD_3D(0x0, 0x77)
#define XE2_GFXPIPE_CMD_3DSTATE_MESH_DISTRIB                               XE2_GFXPIPE_CMD_3D(0x0, 0x78)
#define XE2_GFXPIPE_CMD_3DSTATE_TASK_REDISTRIB                             XE2_GFXPIPE_CMD_3D(0x0, 0x79)
#define XE2_GFXPIPE_CMD_3DSTATE_MESH_SHADER                                XE2_GFXPIPE_CMD_3D(0x0, 0x7A)
#define XE2_GFXPIPE_CMD_3DSTATE_MESH_SHADER_DATA                           XE2_GFXPIPE_CMD_3D(0x0, 0x7B)
#define XE2_GFXPIPE_CMD_3DSTATE_TASK_CONTROL                               XE2_GFXPIPE_CMD_3D(0x0, 0x7C)
#define XE2_GFXPIPE_CMD_3DSTATE_TASK_SHADER                                XE2_GFXPIPE_CMD_3D(0x0, 0x7D)
#define XE2_GFXPIPE_CMD_3DSTATE_TASK_SHADER_DATA                           XE2_GFXPIPE_CMD_3D(0x0, 0x7E)
#define XE2_GFXPIPE_CMD_3DSTATE_URB_ALLOC_MESH                             XE2_GFXPIPE_CMD_3D(0x0, 0x7F)
#define XE2_GFXPIPE_CMD_3DSTATE_URB_ALLOC_TASK                             XE2_GFXPIPE_CMD_3D(0x0, 0x80)
#define XE2_GFXPIPE_CMD_3DSTATE_CLIP_MESH                                  XE2_GFXPIPE_CMD_3D(0x0, 0x81)
#define XE2_GFXPIPE_CMD_3DSTATE_SBE_MESH                                   XE2_GFXPIPE_CMD_3D(0x0, 0x82)
#define XE2_GFXPIPE_CMD_3DSTATE_CPSIZE_CONTROL_BUFFER                      XE2_GFXPIPE_CMD_3D(0x0, 0x83)
#define XE2_GFXPIPE_CMD_3DSTATE_COARSE_PIXEL                               XE2_GFXPIPE_CMD_3D(0x0, 0x89)
#define XE2_GFXPIPE_CMD_3DSTATE_DRAWING_RECTANGLE                          XE2_GFXPIPE_CMD_3D(0x1, 0x00)
#define XE2_GFXPIPE_CMD_3DSTATE_CHROMA_KEY                                 XE2_GFXPIPE_CMD_3D(0x1, 0x04)
#define XE2_GFXPIPE_CMD_3DSTATE_POLY_STIPPLE_OFFSET                        XE2_GFXPIPE_CMD_3D(0x1, 0x06)
#define XE2_GFXPIPE_CMD_3DSTATE_POLY_STIPPLE_PATTERN                       XE2_GFXPIPE_CMD_3D(0x1, 0x07)
#define XE2_GFXPIPE_CMD_3DSTATE_LINE_STIPPLE                               XE2_GFXPIPE_CMD_3D(0x1, 0x08)
#define XE2_GFXPIPE_CMD_3DSTATE_AA_LINE_PARAMETERS                         XE2_GFXPIPE_CMD_3D(0x1, 0x0A)
#define XE2_GFXPIPE_CMD_3DSTATE_MONOFILTER_SIZE                            XE2_GFXPIPE_CMD_3D(0x1, 0x11)
#define XE2_GFXPIPE_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_VS                     XE2_GFXPIPE_CMD_3D(0x1, 0x12)
#define XE2_GFXPIPE_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_HS                     XE2_GFXPIPE_CMD_3D(0x1, 0x13)
#define XE2_GFXPIPE_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_DS                     XE2_GFXPIPE_CMD_3D(0x1, 0x14)
#define XE2_GFXPIPE_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_GS                     XE2_GFXPIPE_CMD_3D(0x1, 0x15)
#define XE2_GFXPIPE_CMD_3DSTATE_PUSH_CONSTANT_ALLOC_PS                     XE2_GFXPIPE_CMD_3D(0x1, 0x16)
#define XE2_GFXPIPE_CMD_3DSTATE_SO_DECL_LIST                               XE2_GFXPIPE_CMD_3D(0x1, 0x17)
#define XE2_GFXPIPE_CMD_3DSTATE_SO_BUFFER                                  XE2_GFXPIPE_CMD_3D(0x1, 0x18)
#define XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POOL_ALLOC                   XE2_GFXPIPE_CMD_3D(0x1, 0x19)
#define XE2_GFXPIPE_CMD_3DSTATE_SAMPLE_PATTERN                             XE2_GFXPIPE_CMD_3D(0x1, 0x1C)
#define XE2_GFXPIPE_CMD_3DSTATE_3D_MODE                                    XE2_GFXPIPE_CMD_3D(0x1, 0x1E)
#define XE2_GFXPIPE_CMD_3DSTATE_SUBSLICE_HASH_TABLE                        XE2_GFXPIPE_CMD_3D(0x1, 0x1F)
#define XE2_GFXPIPE_CMD_3DSTATE_SLICE_TABLE_STATE_POINTERS                 XE2_GFXPIPE_CMD_3D(0x1, 0x20)
#define XE2_GFXPIPE_CMD_3DSTATE_PTBR_TILE_PASS_INFO                        XE2_GFXPIPE_CMD_3D(0x1, 0x22)
#define XE2_GFXPIPE_CMD_3DPRIMITIVE                                        XE2_GFXPIPE_CMD_3D(0x3, 0x0)

// DPCD (DispalyPort configuration data) is GPU-independent standard.
// May be applied elsewhere.
#define DPCD_REG_REV                                                       0x00
// Receiver capability fields read as a 15-byte block from address 0x00. The
// link rate and lane count must be non-zero or the sink's link config is
// rejected and the eDP connector is torn down (no fixed mode, no fb0).
#define DPCD_REG_MAX_LINK_RATE                                             0x01
#define DPCD_REG_MAX_LANE_COUNT                                            0x02
#define DPCD_RECEIVER_CAP_SIZE                                             15
#define DPCD_LINK_RATE_HBR2                                                0x14 // 5.4 Gbps
#define DPCD_LANE_COUNT_4_ENHANCED                                         0x84 // 4 lanes | enhanced framing
// eDP-specific revision block; eDP 1.3 keeps the sink on the MAX_LINK_RATE path
// (no separate link-rate table required).
#define DPCD_REG_EDP_DPCD_REV                                              0x700
#define DPCD_EDP_REV_1_3                                                   0x02
#define DPCD_REG_RECEIVER_ALPM_CAP                                         0x2E
#define DPCD_REG_DSC_SUPPORT                                               0x60
#define DPCD_REG_PSR_SUPPORT                                               0x70
#define DPCD_REG_PANEL_REPLAY_CAP_SUPPORT                                  0xB0
// DisplayPort maximum bandwidth rate.
#define DPCD_REG_DP_LINK_BW_SET                                            0x100
#define DPCD_DP_LINK_RATE_TABLE                                            0x00
#define DPCD_DP_LINK_BW_1_62                                               0x06 // 1.62 Gbit/s per lane
#define DPCD_DP_LINK_BW_2_7                                                0x0A // 2.7  Gbit/s per lane
#define DPCD_DP_LINK_BW_5_4                                                0x14 // 5.4  Gbit/s per lane
#define DPCD_DP_LINK_BW_8_1                                                0x1E // 8.1  Gbit/s per lane
#define DPCD_DP_LINK_BW_10                                                 0x01 // 10   Gbit/s per lane
#define DPCD_DP_LINK_BW_13_5                                               0x04 // 13.5 Gbit/s per lane
#define DPCD_DP_LINK_BW_20                                                 0x02 // 20   Gbit/s per lane

#define DPCD_REG_TRAINING_PATTERN_SET                                      0x102
#define DPCD_TRAINING_PATTERN_DISABLE                                      0
#define DPCD_TRAINING_PATTERN_1                                            1
#define DPCD_TRAINING_PATTERN_2                                            2
#define DPCD_TRAINING_PATTERN_2_CDS                                        3
#define DPCD_TRAINING_PATTERN_3                                            3
#define DPCD_TRAINING_PATTERN_4                                            7
#define DPCD_TRAINING_PATTERN_MASK                                         0x3
#define DPCD_TRAINING_PATTERN_MASK_1_4                                     0xF

#define DPCD_REG_TRAINING_LANE0_SET                                        0x103
#define DPCD_REG_TRAINING_LANE1_SET                                        0x104
#define DPCD_REG_TRAINING_LANE2_SET                                        0x105
#define DPCD_REG_TRAINING_LANE3_SET                                        0x106
#define DPCD_TRAINING_LANEX_SWING_LEVEL_0                                  (0 << 0)
#define DPCD_TRAINING_LANEX_SWING_LEVEL_1                                  (1 << 0)
#define DPCD_TRAINING_LANEX_SWING_LEVEL_2                                  (2 << 0)
#define DPCD_TRAINING_LANEX_SWING_LEVEL_3                                  (3 << 0)

#define DPCD_REG_LANE0_1_STATUS                                            0x202
#define DPCD_REG_LANE2_3_STATUS                                            0x203
#define DPCD_LANEX_X_CR_DONE                                               (1 << 0)
#define DPCD_LANEX_X_CHANNEL_EQ_DONE                                       (1 << 1)
#define DPCD_LANEX_X_SYMBOL_LOCKED                                         (1 << 2)
#define DPCD_LANEX_X_CHANNEL_EQ_BITS                                       (DPCD_LANEX_X_CR_DONE | DPCD_LANEX_X_CHANNEL_EQ_DONE | DPCD_LANEX_X_SYMBOL_LOCKED)

#define DPCD_REG_LANE_ALIGN_STATUS_UPDATED                                 0x204
#define DPCD_INTERLANE_ALIGN_DONE                                          (1 << 0)
#define DPCD_128B132B_DPRX_EQ_INTERLANE_ALIGN_DONE                         (1 << 2) // 2.0 E11
#define DPCD_128B132B_DPRX_CDS_INTERLANE_ALIGN_DONE                        (1 << 3) // 2.0 E11
#define DPCD_128B132B_LT_FAILED                                            (1 << 4) // 2.0 E11
#define DPCD_DOWNSTREAM_PORT_STATUS_CHANGED                                (1 << 6)
#define DPCD_LINK_STATUS_UPDATED                                           (1 << 7)

#define DPCD_SINK_STATUS                                                   0x205
#define DPCD_RECEIVE_PORT_0_STATUS                                         (1 << 0)
#define DPCD_RECEIVE_PORT_1_STATUS                                         (1 << 1)
#define DPCD_STREAM_REGENERATION_STATUS                                    (1 << 2) // 2.0
#define DPCD_INTRA_HOP_AUX_REPLY_INDICATION                                (1 << 3) // 2.0

#define DPCD_TEST_REQUEST                                                  0x218
#define DPCD_TEST_REQUEST_LINK_TRAINING                                    (1 << 0)
#define DPCD_TEST_REQUEST_LINK_VIDEO_PATTERN                               (1 << 1)
#define DPCD_TEST_REQUEST_LINK_EDID_READ                                   (1 << 2)
#define DPCD_TEST_REQUEST_LINK_PHY_TEST_PATTERN                            (1 << 3) // DPCD >= 1.1
#define DPCD_TEST_REQUEST_LINK_FAUX_PATTERN                                (1 << 4) // DPCD >= 1.2
#define DPCD_TEST_REQUEST_LINK_AUDIO_PATTERN                               (1 << 5) // DPCD >= 1.2
#define DPCD_TEST_REQUEST_LINK_AUDIO_DISABLED_VIDEO                        (1 << 6) // DPCD >= 1.2

#define DPCD_TEST_RESPONSE                                                 0x260

#define DPCD_REG_SOURCE_OUI                                                0x300

#define DPCD_REG_SET_POWER                                                 0x600
#define DPCD_SET_POWER_D0                                                  0x1
#define DPCD_SET_POWER_D3                                                  0x1
#define DPCD_SET_POWER_MASK                                                0x1
#define DPCD_SET_POWER_D3_AUX_ON                                           0x1

// EDID address and size is not part of DPCD.
#define DPCD_INTEL_EDID_ADDR                                               0x50
#define DPCD_INTEL_EDID_SIZE                                               128

// https://docs.amd.com/r/en-US/pg199-displayport-tx-subsystem/I2C-Over-AUX-Transactions
//
// Pay attention: Driver natively emulates I2C transactions. Thus, pseudo-I2C
// commands goes through PCI.
#define DPCD_REQ_I2C_WRITE                                                 0x0
#define DPCD_REQ_I2C_READ                                                  0x1
#define DPCD_REQ_I2C_WRITE_STATUS_UPDATE                                   0x2
#define DPCD_REQ_I2C_WRITE_MOT                                             0x4 // Middle of transaction
#define DPCD_REQ_I2C_READ_MOT                                              0x5 // Middle of transaction
#define DPCD_REQ_NATIVE_WRITE                                              0x8
#define DPCD_REQ_NATIVE_READ                                               0x9

// Auxiliary channel entry.
typedef struct {
    uint32_t data[5];      // Auxiliary channel data (5 dwords).
    uint32_t message_size; // Size of AUX message. Encoded in incoming data[0].
    uint32_t ctl;          // DP(X)_AUX_CH_CTL register.
    uint32_t edid_written; // Internal variable.
} xe2_aux_t;

// Cx0 PHY message-bus shadow for a single lane. The host writes 8-bit Cx0
// registers (addressed by a 12-bit address) and reaches a 16-bit SRAM
// indirectly through special register addresses. We mirror back whatever the
// host wrote so post-modeset register verification reads see the right value.
typedef struct {
    uint32_t m2p;           // Last M2P_MSGBUS_CTL value (PENDING cleared).
    uint32_t p2m;           // P2M_MSGBUS_STATUS value (response to last cmd).
    uint8_t  regs[0x1000];  // Cx0 register file (indexed by 12-bit address).
    uint16_t sram[0x10000]; // Indirect 16-bit SRAM.
    uint16_t sram_wr_addr;  // Staged SRAM write address (H<<8 | L).
    uint16_t sram_wr_data;  // Staged SRAM write data (H<<8 | L).
    uint16_t sram_rd_addr;  // Latched SRAM read address.
} xe2_cx0_lane_t;

#define XE2_MEM_SMEM 0 // System memory (accessed via DMA)
#define XE2_MEM_LMEM 1 // Local memory (accessed via VRAM)

typedef struct {
    uint64_t addr;
    uint8_t  type;
} xe2_dma_addr_t;

// Power-related registers. Used to handle
// - PP  (Panel power)
// - PCH (Panel controller hub)
// in the same way.
typedef struct {
    uint32_t control;
    uint32_t status;
    uint32_t on_delays;
    uint32_t off_delays;
} xe2_power_control_t;

// The 3D pipeline state the command streamer accumulates between draws.
// GFXPIPE commands mutate it, 3DPRIMITIVE consumes it. Everything here is
// plain data decoded out of the ring - no Vulkan, no DMA, no device.
//
// State is scoped to the logical ring context (same as the STATE_BASE_ADDRESS
// pointers), so xe2_3dstate_t lives on xe2_submit_ctx_t.
typedef enum {
    XE2_SHADER_VS = 0,
    XE2_SHADER_HS,
    XE2_SHADER_DS,
    XE2_SHADER_GS,
    XE2_SHADER_PS,
    XE2_SHADER_CS,
    XE2_SHADER_STAGE_COUNT
} xe2_shader_kind_t;

// Xe2 dispatches a thread with the constant buffer contents already
// resident in its GRFs, right after the fixed payload registers. A Xe2
// GRF is 64 bytes wide; the constant gather works in 32-byte chunks,
// which is also the unit the Read Length fields are expressed in.
#define XE2_GRF_BYTES         64
#define XE2_GRF_DWORDS        (XE2_GRF_BYTES / 4)
#define XE2_CONST_CHUNK_BYTES 32

// -----------------------------------------------------------
// Shader stages
// -----------------------------------------------------------

// A guest kernel cross-compiled to SPIR-V, plus the payload layout the
// compilation assumed. Both are needed at draw time: the module goes
// into a VkShaderModule, the layout tells the backend which constant
// bytes the module expects to find at which binding.
typedef struct {
    uint32_t*   spirv; // Owned, from spirv_module_finish().
    uint32_t    spirv_nwords;
    rvvm_addr_t kernel_va;     // Kernel address the module was built from.
    uint8_t     push_grf_base; // First GRF the module reads constants from.
    bool        enabled;       // Stage enabled by its 3DSTATE_XS command.
    bool        dirty;         // Kernel changed since the last draw.
} xe2_shader_stage_t;

// -----------------------------------------------------------
// Vertex input (3DSTATE_VERTEX_BUFFERS / _ELEMENTS / _INDEX_BUFFER)
// -----------------------------------------------------------

#define XE2_SHADER_MAX_BINDINGS 16
#define XE2_SHADER_MAX_GRF      128

typedef struct {
    xe2_dma_addr_t addr;
    uint32_t       stride;
    uint32_t       size;
} xe2_vertex_buffer_t;

typedef struct {
    uint32_t binding;
    uint32_t format; // SURFACE_FORMAT enum -> VkFormat
    uint32_t offset;
    uint32_t location; // Shader input location.
} xe2_vertex_element_t;

typedef struct {
    xe2_vertex_buffer_t buffer[XE2_SHADER_MAX_BINDINGS];
    uint32_t            buffer_count;
    uint32_t            topology; // 3DSTATE_VF_TOPOLOGY -> VkPrimitiveTopology

    // Not decoded by any command handler yet: element descriptions come
    // from 3DSTATE_VERTEX_ELEMENTS and the index buffer from
    // 3DSTATE_INDEX_BUFFER, neither of which has a consumer while
    // attribute fetch is unimplemented.
    xe2_vertex_element_t element[XE2_SHADER_MAX_BINDINGS];
    uint32_t             element_count;
    xe2_dma_addr_t       index_addr;
    uint32_t             index_format; // 0=BYTE, 1=WORD, 2=DWORD -> VkIndexType
    bool                 index_valid;
} xe2_vertex_input_t;

// -----------------------------------------------------------
// Fixed function
// -----------------------------------------------------------

// Raw command dwords, kept for whoever decodes them first. Nothing
// populates these yet; the pipeline the backend builds is fixed.
typedef struct {
    uint32_t raster[4];        // 3DSTATE_RASTER
    uint32_t blend[4];         // 3DSTATE_PS_BLEND
    uint32_t depth_stencil[4]; // 3DSTATE_WM_DEPTH_STENCIL
} xe2_ff_state_t;

// -----------------------------------------------------------
// Aggregate state
// -----------------------------------------------------------

// 3DSTATE_CONSTANT_BODY exposes four buffers per stage. The hardware
// concatenates them, in index order, into the pushed GRF payload; Mesa
// uses buffer 0 for the inline uniforms and buffers 1..3 for pushed UBO
// ranges.
#define XE2_CONST_BUFFERS    4
#define XE2_CONST_MAX_VEC4   64
#define XE2_CONST_MAX_DWORDS (XE2_CONST_MAX_VEC4 * 4)
#define XE2_CONST_MAX_BYTES  (XE2_CONST_MAX_DWORDS * 4)

// What a 3DPRIMITIVE asks for, on top of the state around it.
typedef struct {
    uint32_t topology;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} xe2_draw_params_t;

// One entry of 3DSTATE_CONSTANT_BODY.
typedef struct {
    rvvm_addr_t va;          // Buffer address, PPGTT virtual. 0 when unused.
    uint32_t    read_length; // Length in 32-byte chunks, as programmed.
} xe2_const_buffer_t;

// Per-stage constant state: what the command stream programmed, plus the
// payload we gathered out of guest memory for it.
typedef struct {
    xe2_const_buffer_t buffer[XE2_CONST_BUFFERS];

    // Buffer contents concatenated in index order - the exact byte image
    // the hardware would have pushed into the thread's GRFs.
    uint8_t  payload[XE2_CONST_MAX_BYTES];
    uint32_t nbytes;

    bool dirty; // Payload changed since the last draw.
} xe2_push_const_t;

// -----------------------------------------------------------
// RENDER_SURFACE_STATE (decoded subset needed to build a texture)
// -----------------------------------------------------------
//
// Field positions were cross-checked against the trace dumps for this
// device - three independent surfaces (a NULL binding, a linear
// B8G8R8A8 render target, a TILE4 R8G8B8A8 texture) - each bit range
// below reproduces every printed field exactly for all three, rather
// than being copied from a genxml on faith.
typedef enum {
    XE2_SURFTYPE_1D   = 0,
    XE2_SURFTYPE_2D   = 1,
    XE2_SURFTYPE_3D   = 2,
    XE2_SURFTYPE_CUBE = 3,
    XE2_SURFTYPE_NULL = 7,
} xe2_surftype_t;

typedef struct {
    bool           valid; // false for an empty/NULL binding table slot
    xe2_surftype_t type;
    uint32_t       isl_format; // ISL_FORMAT_* - ISL->VkFormat mapping is a backend concern
    uint32_t       tile_mode;  // 0=LINEAR, 3=TILE4 on Xe2
    uint32_t       width;      // pixels
    uint32_t       height;     // pixels
    uint32_t       pitch;      // bytes per row
    xe2_dma_addr_t base;       // translated, dereferenceable
} xe2_surface_state_t;

// Enough headroom for glmark2-es2-drm's handful of texture units per stage.
#define XE2_MAX_BOUND_SURFACES 8

typedef struct {
    xe2_shader_stage_t shader[XE2_SHADER_STAGE_COUNT];
    xe2_push_const_t   consts[XE2_SHADER_STAGE_COUNT];

    // 3DSTATE_BINDING_TABLE_POINTERS_XS, relative to the surface state
    // base (ctx->addr_surf_state). Resolve with xe2_binding_table_base().
    uint32_t binding_table_offset[XE2_SHADER_STAGE_COUNT];
    uint32_t binding_table_entry_count[XE2_SHADER_STAGE_COUNT];

    // Binding table entries decoded into RENDER_SURFACE_STATE, refreshed
    // by xe2_resolve_bindings() right before each draw - see the comment
    // there for why this is lazy rather than done at pointer-decode time.
    xe2_surface_state_t surface[XE2_SHADER_STAGE_COUNT][XE2_MAX_BOUND_SURFACES];

    xe2_vertex_input_t vertex_input;
    xe2_ff_state_t     ff;
    bool               ff_dirty;

    // The draw last handed to the renderer. An identical draw against
    // unchanged state does not need to be handed over again.
    xe2_draw_params_t last_draw;
    bool              last_draw_valid;
} xe2_3dstate_t;

// Registered submission contexts. A doorbell on the shared host-interrupt
// register carries both CT messages and ring-work submissions; rather than
// guess which doorbell is a submission, we track each context's last-
// completed ring tail and complete one only when its LRC RING_TAIL advances.
typedef struct {
    xe2_dma_addr_t pphwsp;    // translated PPHWSP (context page) address
    uint32_t       guc_id;    // Index of GuC engine (not context index)
    uint32_t       last_tail; // ring tail already completed
    bool           valid;
    // Monotonic render-engine completion seqno, published at the LRC status page.
    uint32_t seqno;

    // Registered per-context base addresses for batch buffer.
    rvvm_addr_t addr_general_state;
    rvvm_addr_t addr_surf_state;
    rvvm_addr_t addr_dynamic_state;
    rvvm_addr_t addr_indirect_object;
    rvvm_addr_t addr_instr;
    rvvm_addr_t addr_bindless_surface;
    rvvm_addr_t addr_bindless_sampler;
    rvvm_addr_t addr_binding_table_base;

    xe2_3dstate_t d3d;
} xe2_submit_ctx_t;

typedef struct {
    uint32_t actions_h2g[4];
    uint32_t actions_g2h[4];

    xe2_dma_addr_t memirq_status_addr;
    xe2_dma_addr_t memirq_source_addr;
    uint32_t       memirq_base_ggtt;
    xe2_dma_addr_t ctb_h2g_addr;
    size_t         ctb_h2g_size;
    xe2_dma_addr_t ctb_g2h_addr;
    size_t         ctb_g2h_size;
    xe2_dma_addr_t ctb_h2g_descriptor_addr;
    xe2_dma_addr_t ctb_g2h_descriptor_addr;

    // GuC-to-host interrupt latch. The driver wires MSI-X vector 0 to the
    // register-based top-level handler, which walks DG1_MSTR_TILE_INTR ->
    // GFX_MSTR_IRQ -> GT_INTR_DW -> INTR_IDENTITY_REG to find the source.
    // Latch a pending GuC interrupt and surface it through that chain.
    bool irq_pending;

    // SLPC (GT frequency) shared-data BO, supplied with the SLPC reset
    // request. The host publishes the running state and frequency caps here
    // so the driver's GuC-PC start handshake completes.
    xe2_dma_addr_t slpc_data_addr;
    bool           slpc_data_valid;

    // Latched once the driver asks the GuC to authenticate the HuC firmware.
    // Surfaced through HUC_KERNEL_LOAD_INFO, which the driver polls.
    bool huc_authenticated;
} xe2_guc_t;

typedef struct {
    uint32_t int_ctl; // GEN11_DISPLAY_INT_CTL (enable bit).

    // Per-pipe Display-Engine interrupt registers.
    uint32_t imr[XE2_PIPE_COUNT]; // 1 = masked.
    uint32_t ier[XE2_PIPE_COUNT]; // 1 = enabled.
    uint32_t iir[XE2_PIPE_COUNT]; // Pending bits (write-1-to-clear).
    uint32_t isr[XE2_PIPE_COUNT]; // Raw status read-back storage.

    uint32_t frmcount[XE2_PIPE_COUNT];      // Per-pipe frame counter.
    uint32_t pipe_regs_shadow[0x2000 / 4];  // 0x60000–0x61FFF
    uint32_t plane_regs_shadow[0x8000 / 4]; // 0x70000–0x77FFF

    uint32_t dp_tp_status;

    // Scanout state of pipe A, plane 1 (the universal plane fbcon drives).
    // Captured from the driver's plane register writes; consumed by the
    // refresh tick to blit guest framebuffer memory onto the host window.
    uint32_t plane_ctl;    // PLANE_CTL_1_A    (enable bit, format, tiling)
    uint32_t plane_stride; // PLANE_STRIDE_1_A (line stride in units of 64 bytes)
    uint32_t plane_size;   // PLANE_SIZE_1_A   ((height-1)<<16 | (width-1))
    uint32_t plane_surf;   // PLANE_SURF_1_A   (surface base, GGTT byte offset)
} xe2_display_t;

typedef struct {
    // These sizes come from firmware blob.
    uint8_t main[0x470C];
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
} xe2_firmware_t;

typedef struct {
    spirv_module_t    mod;
    xe2_shader_kind_t stage;

    uint32_t void_ty;
    uint32_t fn_ty;
    uint32_t fty;  // float32
    uint32_t v4ty; // vec4 float
    uint32_t func;

    // Register file. grf_var is filled lazily, grf_written tracks which
    // registers the kernel has defined so far in program order.
    uint32_t grf_var[XE2_SHADER_MAX_GRF];
    bool     grf_written[XE2_SHADER_MAX_GRF];
    uint32_t push_grf_base;

    // Pushed constants, bound as a uniform block (see gpu-vulkan.h for
    // the descriptor layout the backend builds to match).
    uint32_t const_var;      // The block variable.
    uint32_t const_elem_ptr; // Pointer to one float inside it.

    uint32_t position_out; // BuiltIn Position (VS).
    uint32_t color_out;    // Location 0 (PS).
    bool     wrote_output;

    uint32_t entry_iface[8];
    size_t   entry_iface_n;

    bool saw_eot;
} xe2_spirv_ctx_t;

typedef struct {
    rvvm_pci_func_t* pci_func;
    spinlock_t       lock;
    uint32_t         forcewake_gsc;
    uint32_t         forcewake_gt_mtl;
    uint32_t         forcewake_renderer;
    uint32_t         gt_gdrst;
    uint32_t         pll_enable;
    uint32_t         dbuf_ctl[4];

    xe2_power_control_t pp;
    xe2_power_control_t pp_pch;
    uint32_t            dc_state;

    uint32_t steer_semaphore;
    uint32_t wopcm_size;
    uint32_t wopcm_offset;
    uint32_t wopcm_locked;

    xe2_aux_t aux[1]; // We assume one display with one AUX channel.

    xe2_cx0_lane_t cx0[XE2_CX0_LANE_TOTAL]; // Cx0 PHY per-lane message bus.
    uint32_t       port_clock_ctl;          // XELPDP_PORT_CLOCK_CTL (PORT_A).
    uint32_t       port_buf_ctl1;           // XELPDP_PORT_BUF_CTL1 (PORT_A).
    uint32_t       port_buf_ctl2;           // XELPDP_PORT_BUF_CTL2 (PORT_A).
    uint32_t       trans_ddi_func_ctl;      // TRANS_DDI_FUNC_CTL (transcoder A).
    uint32_t       transconf;               // TRANSCONF (transcoder A).

    uint32_t spi_address;
    uint32_t spi_trigger;

    rvvm_addr_t dma_0;
    rvvm_addr_t dma_1;
    uint32_t    dma_copy_size;

    xe2_submit_ctx_t ctx[XE2_MAX_CONTEXTS];

    xe2_guc_t      guc;
    xe2_display_t  display;
    xe2_firmware_t firmware;

    // Host display device. NULL when running headless (-nogui / -nogpu display).
    rvvm_fbdev_t* fbdev;

    // Page table entries.
    uint64_t* ggtt_pte;
    // Lower PTE addresses. Comes from splitted
    // 64-bit MMIO request into two 32-bit ones.
    uint32_t* ggtt_lo_addrs;
    // Validity map.
    bool* ggtt_pte_valid;

    uint8_t* vram;

    gpu_vulkan_ctx_t* vulkan_ctx;
    xe2_spirv_ctx_t   spirv_ctx;

    bool     draw_submitted;
    uint64_t last_draw_tick;
    uint64_t scanout_tick;

    // The DMC loader writes a per-firmware list of (register, value) pairs and
    // later verifies the registers read back those values. Shadow the two DMC
    // register windows so those writes stick. These hold no other state.
    uint8_t dmc_mmio_5f[0x1000]; // 0x5F000..0x5FFFF (pipe DMCs)
    uint8_t dmc_mmio_8f[0x1000]; // 0x8F000..0x8FFFF (main DMC)
} xe2_dev_t;

// -----------------------------------------------------------
// Utilities, handy functions
// -----------------------------------------------------------



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
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x09, 0xE5, 0xA3, 0x07, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B, 0x01,
    0x04, 0x95, 0x22, 0x13, 0x78, 0x02, 0xB0, 0x90, 0x97, 0x58, 0x54, 0x92, 0x26, 0x1D, 0x50, 0x54, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x3B, 0x37, 0x80,
    0xB8, 0x70, 0x38, 0x28, 0x40, 0x30, 0x20, 0x36, 0x00, 0x58, 0xC1, 0x10, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x00,
    0x42, 0x4F, 0x45, 0x20, 0x43, 0x51, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFE, 0x00, 0x4E,
    0x54, 0x31, 0x35, 0x36, 0x46, 0x48, 0x4D, 0x2D, 0x4E, 0x34, 0x31, 0x0A, 0x00, 0x27,
};

// Synthetic VBT (Video BIOS Table) served over the SPI flash window. The driver
// needs it to enumerate display outputs; without it port setup is guessed and
// warns. This minimal image declares one eDP child device on PORT_A (DP A), so
// the eDP connector is created and paired with the EDID served over AUX.
// Layout: vbt_header(48) + bdb_header(22) + BDB_GENERAL_DEFINITIONS block. The
// "$VBT" signature sits 4-byte aligned at flash offset 0 where the driver scans.
static const uint8_t xe2_vbt[] = {
    0x24, 0x56, 0x42, 0x54, 0x20, 0x52, 0x56, 0x56, 0x4D, 0x2D, 0x58, 0x45, 0x32, 0x20, 0x45, 0x4D, 0x55, 0x4C,
    0x00, 0x00, 0x00, 0x01, 0x30, 0x00, 0x7A, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x42, 0x49, 0x4F, 0x53, 0x5F, 0x44,
    0x41, 0x54, 0x41, 0x5F, 0x42, 0x4C, 0x4F, 0x43, 0x4B, 0x20, 0x07, 0x01, 0x16, 0x00, 0x4A, 0x00, 0x02, 0x31,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x08, 0x00, 0xC6, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static inline rvvm_addr_t xe2_concat_lohi(uint32_t lo, uint32_t hi)
{
    return (rvvm_addr_t)lo | (rvvm_addr_t)hi << 32;
}

// Return the little-endian 32-bit flash word at the latched SPI address, served
// from the synthetic VBT image at flash offset 0 (zero-filled beyond its end).
static uint32_t xe2_spi_read32(xe2_dev_t* xe2)
{
    uint32_t addr = xe2->spi_address;
    uint32_t word = 0;
    for (uint32_t i = 0; i < 4; i++) {
        if (addr + i < sizeof(xe2_vbt)) {
            word |= (uint32_t)xe2_vbt[addr + i] << (i * 8);
        }
    }
    return word;
}

static void xe2_remove(rvvm_reg_dev_t* dev)
{
    xe2_dev_t* xe2 = rvvm_region_data(dev);

    // Tear the renderer down first: it owns a worker thread that blits
    // into the framebuffer memory freed further down.
    //
    // BUG: Corrupted synchronization, render is hangs in
    // "in progress state" while destroyed.
    gpu_vulkan_destroy(xe2->vulkan_ctx);

    xe2->vulkan_ctx = NULL;

    for (size_t i = 0; i < XE2_MAX_CONTEXTS; ++i) {
        for (size_t s = 0; s < XE2_SHADER_STAGE_COUNT; ++s) {
            free(xe2->ctx[i].d3d.shader[s].spirv);
        }
    }

    if (xe2->fbdev) {
        rvvm_fbdev_dec_ref(xe2->fbdev);
    }
    vma_free(xe2->ggtt_pte_valid, XE2_GGTT_PAGES * sizeof(bool));
    vma_free(xe2->ggtt_lo_addrs, XE2_GGTT_PAGES * sizeof(uint32_t));
    vma_free(xe2->ggtt_pte, XE2_GGTT_PAGES * sizeof(uint64_t));
    vma_free(xe2->vram, XE2_VRAM_SIZE);
    free(xe2);
}

static void xe2_remove_vram(rvvm_reg_dev_t* dev)
{
    UNUSED(dev);
}

// -----------------------------------------------------------
// Display scanout
// -----------------------------------------------------------



// Bits of a pipe's DE_PIPE interrupt that are currently live, i.e. pending,
// enabled and not masked.
static inline uint32_t xe2_display_pipe_live(xe2_dev_t* xe2, uint32_t pipe)
{
    return xe2->display.iir[pipe] & xe2->display.ier[pipe] & ~xe2->display.imr[pipe];
}

// True if any pipe has a live (pending, enabled, unmasked) interrupt bit.
static inline bool xe2_display_pending(xe2_dev_t* xe2)
{
    for (uint32_t pipe = 0; pipe < XE2_PIPE_COUNT; pipe++) {
        if (xe2_display_pipe_live(xe2, pipe)) {
            return true;
        }
    }
    return false;
}

// Resolve one GGTT page to a readable host pointer, without the verbose logging
// of xe2_ggtt_translate (this runs per-page, every frame). Returns the number of
// bytes readable from the returned pointer before the next page in *avail.
//
// \return Obtained DMA pointer, that must be freed with `rvvm_pci_end_dma()` when no longer needed.
static uint8_t* xe2_scanout_page_dma(xe2_dev_t* xe2, rvvm_addr_t ggtt, size_t* avail)
{
    uint64_t page = ggtt >> 12;
    uint64_t off  = ggtt & 0xFFF;
    *avail        = 0x1000 - off;
    if (page >= XE2_GGTT_PAGES) {
        return NULL;
    }

    uint64_t pte = xe2->ggtt_pte[page];
    if (!(pte & 1)) { // Not present.
        return NULL;
    }

    uint64_t addr = (pte & 0x0000FFFFFFFFF000ULL) + off;
    if (pte & 2) { // Local (VRAM) memory.
        if (unlikely(addr + *avail > XE2_VRAM_SIZE)) {
            return NULL;
        }
        return xe2->vram + addr;
    }
    // System memory, reachable through the guest's DMA window.
    return rvvm_pci_get_dma(xe2->pci_func, addr, *avail);
}

#define XE2_DRAW_IDLE_TICKS 60

// Present pipe-A plane 1 onto the host window. The driver programs a linear
// XRGB8888 framebuffer (fbcon) into a GPU buffer object whose surface address is
// a GGTT offset; resolve it page by page and blit it into the window's VRAM,
// then point the scanout at it. No-op when headless or the plane is disabled.
static void xe2_scanout(xe2_dev_t* xe2)
{
    if (!xe2->fbdev) {
        return;
    }

    xe2->scanout_tick++;

    uint32_t ctl = xe2->display.plane_ctl;
    if (unlikely(!(ctl & XE2_REG_PLANE_CTL_X_ENABLE_MASK))) {
        return;
    }

    // Only linear surfaces are blitted directly; tiled layouts (bits 12:10 != 0)
    // would need detiling, which fbcon never uses, so skip them.
    if (unlikely(xe2_reg_field_get(XE2_REG_PLANE_CTL_X_TILED_MASK, ctl))) {
        return;
    }

    uint32_t width  = (xe2->display.plane_size & 0x1FFF) + 1;
    uint32_t height = ((xe2->display.plane_size >> 16) & 0x1FFF) + 1;
    uint32_t stride = (xe2->display.plane_stride & 0x3FF) * 64;

    if (unlikely(!width || !height || !stride)) {
        return;
    }

    // Pixel format from PLANE_CTL[27:24]; the order bit selects RGB vs BGR.
    rvvm_rgb_t format;
    switch ((ctl >> 24) & 0xF) {
        case 14:
            format = RVVM_RGB_RGB565;
            break; // RGB_565
        case 2:
            format = RVVM_RGB_XRGB2101010;
            break; // XRGB_2101010
        case 4:    // XRGB_8888
        default:
            format = (ctl & XE2_REG_PLANE_CTL_X_ORDER_RGBX_MASK) ? RVVM_RGB_XBGR8888 : RVVM_RGB_XRGB8888;
            break;
    }

    size_t   vram_size = 0;
    uint8_t* dst       = rvvm_fbdev_get_vram(xe2->fbdev, &vram_size);
    size_t   needed    = (size_t)stride * height;
    if (!dst || needed > vram_size) {
        return;
    }
    // No new 3DPRIMITIVE for a while: the guest stopped driving the 3D
    // pipeline (app exited, compositor switched back to fbcon, context
    // torn down, etc). Fall back to the plane's raw buffer rather than
    // freezing on a stale Vulkan frame forever.
    if (xe2->draw_submitted && (xe2->scanout_tick - xe2->last_draw_tick) > XE2_DRAW_IDLE_TICKS) {
        xe2->draw_submitted = false;
    }

    if (xe2->draw_submitted) {
        // rvvm_info("Draw submitted, rendering frame");
        uint32_t   out_width  = 0U;
        uint32_t   out_height = 0U;
        uint32_t   out_stride = 0U;
        rvvm_rgb_t out_format = {0};
        if (xe2->vulkan_ctx
            && gpu_vulkan_render_frame(xe2->vulkan_ctx, width, height, dst, vram_size, stride, format, &out_width,
                                       &out_height, &out_stride, &out_format)) {
            rvvm_fb_t fb = {
                .buffer = dst,
                .width  = out_width,
                .height = out_height,
                .stride = out_stride,
                .format = out_format,
            };
            rvvm_fbdev_set_scanout(xe2->fbdev, &fb);
        }
    } else {
        uint64_t surf   = xe2->display.plane_surf & ~0xFFFULL;
        size_t   copied = 0;
        while (copied < needed) {
            size_t   avail = 0;
            uint8_t* dma   = xe2_scanout_page_dma(xe2, surf + copied, &avail);
            size_t   chunk = (avail < needed - copied) ? avail : needed - copied;
            if (dma) {
                memcpy(dst + copied, dma, chunk);
            } else {
                memset(dst + copied, 0, chunk);
            }
            copied += chunk;
            rvvm_pci_end_dma(xe2->pci_func, dma);
        }

        rvvm_fb_t fb = {
            .buffer = dst,
            .width  = width,
            .height = height,
            .stride = stride,
            .format = format,
        };
        rvvm_fbdev_set_scanout(xe2->fbdev, &fb);
    }
}

// Periodic display refresh callback, invoked by RVVM at roughly 60 Hz. For
// every pipe with vblank enabled, advance the frame counter and raise its
// vblank interrupt; raise flip-done too when that source is enabled, which
// completes any armed page-flip. If any pipe becomes live, fire the MSI.
static void xe2_update(rvvm_reg_dev_t* dev)
{
    xe2_dev_t* xe2 = rvvm_region_data(dev);
    spin_lock(&xe2->lock);

    // Refresh the on-screen image from the guest's scanout buffer.
    xe2_scanout(xe2);

    bool raise = false;
    for (uint32_t pipe = 0; pipe < XE2_PIPE_COUNT; pipe++) {
        bool vblank_en = (xe2->display.ier[pipe] & XE2_REG_DE_PIPE_VBLANK_MASK)
                      && !(xe2->display.imr[pipe] & XE2_REG_DE_PIPE_VBLANK_MASK);
        bool flip_done_en = (xe2->display.ier[pipe] & XE2_REG_DE_PIPE_FLIP_DONE_MASK)
                         && !(xe2->display.imr[pipe] & XE2_REG_DE_PIPE_FLIP_DONE_MASK);

        if (vblank_en) {
            xe2->display.frmcount[pipe]++;
            xe2->display.iir[pipe] |= XE2_REG_DE_PIPE_VBLANK_MASK;
        }
        if (flip_done_en) {
            xe2->display.iir[pipe] |= XE2_REG_DE_PIPE_FLIP_DONE_MASK;
        }
        if (xe2_display_pipe_live(xe2, pipe)) {
            raise = true;
        }
    }

    if (raise) {
        rvvm_pci_send_irq(xe2->pci_func, 0);
    }

    spin_unlock(&xe2->lock);

    // Push the refreshed scanout to the host window (draws & polls input).
    // Done outside the device lock so the GUI redraw can't stall MMIO.
    if (xe2->fbdev) {
        rvvm_fbdev_update(xe2->fbdev);
    }
}

// -----------------------------------------------------------
// Memory model (GGTT, PPGTT, DMA)
// -----------------------------------------------------------



static inline void xe2_ggtt_mmio_write(xe2_dev_t* xe2, uint32_t offset, uint32_t value)
{
    uint64_t idx = (offset - XE2_GGTT_MMIO_BASE) / 8;
    bool     hi  = offset & 4;

    if (!hi) {
        xe2->ggtt_lo_addrs[idx]  = value;
        xe2->ggtt_pte_valid[idx] = true;
        return;
    }

    if (xe2->ggtt_pte_valid[idx]) {
        rvvm_addr_t pte    = xe2_concat_lohi(xe2->ggtt_lo_addrs[idx], value);
        xe2->ggtt_pte[idx] = pte;
    }
}

static inline uint32_t xe2_dma_read_32(xe2_dev_t* xe2, xe2_dma_addr_t dma, size_t off)
{
    if (dma.type == XE2_MEM_LMEM) {
        if (unlikely(dma.addr + off + 4 > XE2_VRAM_SIZE)) {
            return 0;
        }
        return read_uint32_le(xe2->vram + dma.addr + off);
    } else {
        uint32_t* ptr = rvvm_pci_get_dma(xe2->pci_func, dma.addr + off, 4);
        uint32_t  val = ptr ? *ptr : 0;
        rvvm_pci_end_dma(xe2->pci_func, ptr);
        return val;
    }
}

static inline void xe2_dma_read_many(xe2_dev_t* xe2, xe2_dma_addr_t dma, void* out, uint32_t n)
{
    uint32_t total = n * sizeof(uint32_t);

    if (dma.type == XE2_MEM_LMEM) {
        if (unlikely(dma.addr + total > XE2_VRAM_SIZE)) {
            return;
        }
        memcpy(out, xe2->vram + dma.addr, total);
    } else {
        uint32_t* ptr = rvvm_pci_get_dma(xe2->pci_func, dma.addr, 4);
        memcpy(out, ptr, total);
        rvvm_pci_end_dma(xe2->pci_func, ptr);
    }
}

// Bulk read of an arbitrary byte range. Unlike xe2_dma_read_many() this
// asks the PCI layer to map the whole range, so it stays correct for
// reads bigger than a dword, and copies whatever prefix is mappable when
// the range crosses into unmapped memory.
static void xe2_dma_read_bytes(xe2_dev_t* xe2, xe2_dma_addr_t dma, void* out, size_t size)
{
    if (dma.type == XE2_MEM_LMEM) {
        if (unlikely(dma.addr + size > XE2_VRAM_SIZE)) {
            return;
        }
        memcpy(out, xe2->vram + dma.addr, size);
        return;
    }

    uint8_t* dst = (uint8_t*)out;
    size_t   off = 0;
    while (off < size) {
        size_t avail = size - off;
        void*  ptr   = rvvm_pci_get_dma_part(xe2->pci_func, dma.addr + off, &avail);
        if (unlikely(!ptr || !avail)) {
            rvvm_pci_end_dma(xe2->pci_func, ptr);
            return;
        }
        memcpy(dst + off, ptr, avail);
        rvvm_pci_end_dma(xe2->pci_func, ptr);
        off += avail;
    }
}

static void xe2_dma_write_32(xe2_dev_t* xe2, xe2_dma_addr_t dma, size_t off, uint32_t msg)
{
    if (dma.type == XE2_MEM_LMEM) {
        if (unlikely(dma.addr + off + 4 > XE2_VRAM_SIZE)) {
            rvvm_info("%s: Failed", __FUNCTION__);
            return;
        }
        write_uint32_le(xe2->vram + dma.addr + off, msg);
    } else {
        uint32_t* ptr = rvvm_pci_get_dma(xe2->pci_func, dma.addr + off, 4);
        if (likely(ptr)) {
            *ptr = msg;
        }
        rvvm_pci_end_dma(xe2->pci_func, ptr);
    }
}

// Add/subtract an offset to the DMA address. DMA type left unchanged.
static inline xe2_dma_addr_t xe2_dma_offset(xe2_dma_addr_t dma, ssize_t off)
{
    return (xe2_dma_addr_t) {.addr = dma.addr + off, .type = dma.type};
}

static rvvm_addr_t xe2_read_pte_lmem(xe2_dev_t* xe2, rvvm_addr_t phys_addr)
{
    // Table levels (PML4/PDPT/PD/PT) are always LMEM-resident.
    xe2_dma_addr_t dma = {.addr = phys_addr & ~0xFFFULL, .type = XE2_MEM_LMEM};
    uint32_t       lo  = xe2_dma_read_32(xe2, dma, phys_addr & 0xFFFULL);
    uint32_t       hi  = xe2_dma_read_32(xe2, dma, (phys_addr & 0xFFFULL) + 4);
    return xe2_concat_lohi(lo, hi);
}

static inline xe2_dma_addr_t xe2_ggtt_translate(xe2_dev_t* xe2, rvvm_addr_t ggtt)
{
    uint64_t idx = ggtt >> 12;
    uint64_t off = ggtt & 0xFFF;
    uint64_t pte = xe2->ggtt_pte[idx];

    if (unlikely(!(pte & 1))) {
        rvvm_warn("PTE 0x%" PRIu64 " is invalid!", pte);
        return (xe2_dma_addr_t) {.addr = 0, .type = 0};
    }

    return (xe2_dma_addr_t) {
        .addr = (pte & 0x0000FFFFFFFFF000ULL) + off,
        .type = (pte & 2) ? XE2_MEM_LMEM : XE2_MEM_SMEM,
    };
}

// This implements multi-level PPGTT page walk. We have 4 or 5 translation
// levels, where most often 4 is enough for 2 MiB pages.
//
// I hope this translation will not be used in GPU rendering hotspots due
// to ton branching and lookups.
static inline xe2_dma_addr_t xe2_ppgtt_translate(xe2_dev_t* xe2, rvvm_addr_t pdp4, rvvm_addr_t va)
{
    // 48-bit addressing mode, where
    //
    // PML4      PDP        PD        PT        offset
    // 000000000 0100000000 000000001 100000001 100100100000
    // |         |        |         |         |            |
    // 47        39       30        21        12           0
    rvvm_addr_t root_addr = (pdp4 & ~0xFFFULL) + 0 * 8;
    rvvm_addr_t root_e    = xe2_read_pte_lmem(xe2, root_addr);
    if (unlikely(!(root_e & 1))) {
        rvvm_warn("PPGTT: Root L4 entry not present (VA=0x%lx)", va);
        return (xe2_dma_addr_t) {0};
    }
    rvvm_addr_t pml4_base = root_e & ~0xFFFULL;

    rvvm_addr_t pml4_idx = (va >> 39) & 0x1FF; // 9 bits
    rvvm_addr_t pdpt_idx = (va >> 30) & 0x1FF; // 9 bits
    rvvm_addr_t pd_idx   = (va >> 21) & 0x1FF; // 9 bits
    rvvm_addr_t pt_idx   = (va >> 12) & 0x1FF; // 9 bits
    rvvm_addr_t offset   = va & 0xFFF;         // 12 bits

    rvvm_addr_t pml4_addr = (pml4_base & ~0xFFFULL) + pml4_idx * 8;
    rvvm_addr_t pml4e     = xe2_read_pte_lmem(xe2, pml4_addr);
    if (unlikely(!(pml4e & 1))) {
        rvvm_warn("PPGTT: PML4e not present (VA=0x%lx)", va);
        return (xe2_dma_addr_t) {0};
    }

    rvvm_addr_t pdpt_addr = (pml4e & ~0xFFFULL) + pdpt_idx * 8;
    rvvm_addr_t pdpte     = xe2_read_pte_lmem(xe2, pdpt_addr);
    if (unlikely(!(pdpte & 1))) {
        rvvm_warn("PPGTT: PDPTe not present (VA=0x%lx)", va);
        return (xe2_dma_addr_t) {0};
    }

    // 1 GiB huge page
    if (pdpte & (1 << 7)) {
        uint64_t phys = (pdpte & ~0x3FFFFFFFULL) + (va & 0x3FFFFFFF);
        return (xe2_dma_addr_t) {
            .addr = phys,
            .type = (pdpte & (1ULL << 11)) ? XE2_MEM_LMEM : XE2_MEM_SMEM,
        };
    }

    rvvm_addr_t pd_addr = (pdpte & ~0xFFFULL) + pd_idx * 8;
    rvvm_addr_t pde     = xe2_read_pte_lmem(xe2, pd_addr);
    if (unlikely(!(pde & 1))) {
        rvvm_warn("PPGTT: PDE not present (VA=0x%lx)", va);
        return (xe2_dma_addr_t) {0};
    }

    // 2 MiB huge page
    if (likely(pde & (1 << 7))) {
        rvvm_addr_t phys = (pde & ~0x1FFFFFULL) + (va & 0x1FFFFF);
        return (xe2_dma_addr_t) {
            .addr = phys,
            .type = (pde & (1 << 11)) ? XE2_MEM_LMEM : XE2_MEM_SMEM,
        };
    }

    rvvm_addr_t pt_addr = (pde & ~0xFFFULL) + pt_idx * 8;
    rvvm_addr_t pte     = xe2_read_pte_lmem(xe2, pt_addr);
    if (unlikely(!(pte & 1))) {
        rvvm_warn("PPGTT: PTE not present (VA=0x%lx)", va);
        return (xe2_dma_addr_t) {0};
    }

    return (xe2_dma_addr_t) {
        .addr = (pte & ~0xFFFULL) + offset,
        .type = (pte & (1 << 11)) ? XE2_MEM_LMEM : XE2_MEM_SMEM,
    };
}

// -----------------------------------------------------------
// DisplayPort configuration
// -----------------------------------------------------------



// Pack an AUX reply: a leading ACK header byte (0x00) followed by the payload,
// big-endian within each of the five 32-bit AUX data registers (the same byte
// order the EDID-over-AUX path uses).
static inline void xe2_aux_reply(xe2_aux_t* aux, const uint8_t* payload, size_t n)
{
    uint8_t buf[20] = {0};
    for (size_t i = 0; i < n && i + 1 < sizeof(buf); i++) {
        buf[1 + i] = payload[i];
    }
    for (size_t d = 0; d < 5; d++) {
        aux->data[d] = read_uint32_be_m(&buf[d * 4]);
    }
}

static inline void xe2_aux_reply_uint8(xe2_aux_t* aux, uint8_t cmd)
{
    xe2_aux_reply(aux, &cmd, 1);
}

static inline void xe2_aux_reply_uint16(xe2_aux_t* aux, uint16_t cmd)
{
    xe2_aux_reply(aux, (const uint8_t*)&cmd, 2);
}

static inline void xe2_dpcd_aux_config(uint32_t cmd, uint32_t request, uint32_t size, xe2_aux_t* aux)
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

    rvvm_info("AUX cmd: 0x%x", cmd);

    switch (cmd) {
        case DPCD_REG_REV: {
            // Full receiver capability block. The driver reads it in one go and
            // rejects the link (tearing down the eDP connector) unless the rev,
            // link rate and lane count are all non-zero.
            uint8_t caps[DPCD_RECEIVER_CAP_SIZE] = {0};
            caps[DPCD_REG_REV]                   = 0x12; // DPCD rev 1.2
            caps[DPCD_REG_MAX_LINK_RATE]         = DPCD_LINK_RATE_HBR2;
            caps[DPCD_REG_MAX_LANE_COUNT]        = DPCD_LANE_COUNT_4_ENHANCED;
            xe2_aux_reply(aux, caps, sizeof(caps));
            break;
        }
        case DPCD_REG_EDP_DPCD_REV:
            // eDP capability block; rev 1.3 keeps the sink on the MAX_LINK_RATE
            // path, so no separate supported-link-rate table is needed.
            xe2_aux_reply_uint8(aux, DPCD_EDP_REV_1_3);
            break;
        case DPCD_REG_RECEIVER_ALPM_CAP:
            // DP_ALPM_CAP
            xe2_aux_reply_uint8(aux, 1);
            break;
        case DPCD_REG_DSC_SUPPORT:
            // DP_DSC_DECOMPRESSION_IS_SUPPORTED & DP_DSC_PASSTHROUGH_IS_SUPPORTED
            xe2_aux_reply_uint8(aux, 3);
            break;
        case DPCD_REG_PSR_SUPPORT:
            // DP_PSR_IS_SUPPORTED
            xe2_aux_reply_uint8(aux, 1);
            break;
        case DPCD_REG_PANEL_REPLAY_CAP_SUPPORT:
            // DP_PANEL_REPLAY_SUPPORT
            xe2_aux_reply_uint8(aux, 1);
            break;
        case DPCD_REG_SOURCE_OUI:
            // Probably hardcoded value (0xAA01 after write).
            xe2_aux_reply_uint16(aux, 0x01AA);
            break;
        case DPCD_REG_DP_LINK_BW_SET:
            // This is the Church of Satan where Anton LaVey, the
            // high priest, says, "Live is evil spelt backwards".
            xe2_aux_reply_uint8(aux, DPCD_DP_LINK_BW_5_4);
            break;
        case DPCD_REG_SET_POWER:
            xe2_aux_reply_uint8(aux, DPCD_SET_POWER_D0);
            break;
        case DPCD_REG_TRAINING_PATTERN_SET:
            xe2_aux_reply_uint8(aux, DPCD_TRAINING_PATTERN_2_CDS);
            break;
        case DPCD_REG_TRAINING_LANE0_SET:
            xe2_aux_reply_uint8(aux, DPCD_TRAINING_LANEX_SWING_LEVEL_2);
            break;
        case DPCD_REG_LANE0_1_STATUS: {
            // DPCD driver expects 6-byte reply pack.
            uint8_t status[6] = {
                /* 0x202 */ [0] = DPCD_LANEX_X_CHANNEL_EQ_BITS,
                /* 0x203 */[1]  = DPCD_LANEX_X_CHANNEL_EQ_BITS,
                /* 0x204 */[2]  = DPCD_INTERLANE_ALIGN_DONE,
                /* 0x205 */[3]  = DPCD_RECEIVE_PORT_0_STATUS,
                /* 0x206 */[4]  = 0, // Adjust request for lane 0/1
                /* 0x207 */[5]  = 0  // Adjust request for lane 2/3
            };
            xe2_aux_reply(aux, status, sizeof(status));
            break;
        }
        case DPCD_TEST_REQUEST:
            xe2_aux_reply_uint8(aux, DPCD_TEST_REQUEST_LINK_TRAINING);
            break;
        case DPCD_INTEL_EDID_ADDR: {
            uint32_t header  = read_uint32_be_m(&xe2_edid[aux->edid_written + 0]) >> 8;
            uint32_t chunk_1 = read_uint32_be_m(&xe2_edid[aux->edid_written + 3]);
            uint32_t chunk_2 = read_uint32_be_m(&xe2_edid[aux->edid_written + 7]);
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
            // Otherwise enable everything.
            xe2_aux_reply_uint8(aux, 0xFF);
            break;
    }
}

static inline void xe2_emulate_aux_transfer(xe2_dev_t* xe2, size_t aux_no)
{
    xe2_aux_t* aux = &xe2->aux[aux_no];
    uint32_t   cmd = aux->data[0];

    uint32_t request = xe2_reg_field_get(xe2_reg_genmask(31, 28), cmd);
    uint32_t address = xe2_reg_field_get(xe2_reg_genmask(27, 8), cmd);
    uint32_t size    = xe2_reg_field_get(xe2_reg_genmask(4, 0), cmd) + 2;
    // Linux manipulates with AUX transfer size taking header (1 byte)
    // into the account. Finally, GPU returns size equal len(payload) + 2(headers).
    uint32_t payload_size = size - 1;

    xe2->aux[0].ctl &= ~xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_MSG_SIZE_MASK, 0xF);
    xe2->aux[0].ctl |= xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_MSG_SIZE_MASK, size);

    xe2_dpcd_aux_config(address, request, payload_size, aux);
}

// -----------------------------------------------------------
// Cx0 PHY
// -----------------------------------------------------------



// Apply a committed Cx0 register write that targets the indirect SRAM access
// registers. Staging registers latch their bytes; a committed write to the
// write-data low byte performs the actual 16-bit SRAM store.
static inline void xe2_cx0_sram_write(xe2_cx0_lane_t* lane, uint32_t address, uint8_t value)
{
    switch (address) {
        case XE2_CX0_REG_SRAM_WR_ADDRESS_H:
            lane->sram_wr_addr = (lane->sram_wr_addr & 0x00FF) | ((uint16_t)value << 8);
            break;
        case XE2_CX0_REG_SRAM_WR_ADDRESS_L:
            lane->sram_wr_addr = (lane->sram_wr_addr & 0xFF00) | value;
            break;
        case XE2_CX0_REG_SRAM_WR_DATA_H:
            lane->sram_wr_data = (lane->sram_wr_data & 0x00FF) | ((uint16_t)value << 8);
            break;
        case XE2_CX0_REG_SRAM_WR_DATA_L:
            // Committing the low byte performs the 16-bit SRAM store.
            lane->sram_wr_data             = (lane->sram_wr_data & 0xFF00) | value;
            lane->sram[lane->sram_wr_addr] = lane->sram_wr_data;
            break;
        case XE2_CX0_REG_SRAM_RD_ADDRESS_H:
            lane->sram_rd_addr = (lane->sram_rd_addr & 0x00FF) | ((uint16_t)value << 8);
            break;
        case XE2_CX0_REG_SRAM_RD_ADDRESS_L:
            lane->sram_rd_addr = (lane->sram_rd_addr & 0xFF00) | value;
            break;
        default:
            break;
    }
}

// Return the byte a Cx0 register read should yield. The SRAM read-data
// registers expose the high/low byte of the latched 16-bit SRAM word; all
// other registers read back from the Cx0 register file.
static inline uint8_t xe2_cx0_reg_read(xe2_cx0_lane_t* lane, uint32_t address)
{
    switch (address) {
        case XE2_CX0_REG_SRAM_RD_DATA_H:
            return lane->sram[lane->sram_rd_addr] >> 8;
        case XE2_CX0_REG_SRAM_RD_DATA_L:
            return lane->sram[lane->sram_rd_addr] & 0xFF;
        default:
            return lane->regs[address & 0xFFF];
    }
}

// Emulate a single Cx0 message-bus transaction. Called when the host writes
// M2P_MSGBUS_CTL with TRANSACTION_PENDING set. The pending bit is cleared in
// the stored M2P value so the host's poll-for-clear succeeds, and the matching
// P2M response is staged for committed/read commands.
static inline void xe2_cx0_msgbus_transaction(xe2_cx0_lane_t* lane, uint32_t cmd)
{
    if (cmd & XE2_REG_CX0_M2P_TRANSACTION_RESET_MASK) {
        // Bus reset is self-clearing: read M2P/P2M back as cleared.
        lane->m2p = 0;
        lane->p2m = 0;
        return;
    }

    uint32_t command = xe2_reg_field_get(XE2_REG_CX0_M2P_COMMAND_TYPE_MASK, cmd);
    uint32_t address = xe2_reg_field_get(XE2_REG_CX0_M2P_ADDRESS_MASK, cmd);
    uint8_t  payload = xe2_reg_field_get(XE2_REG_CX0_M2P_DATA_MASK, cmd);

    // Store the command word with PENDING cleared so the host poll succeeds.
    lane->m2p = cmd & ~XE2_REG_CX0_M2P_TRANSACTION_PENDING_MASK;

    switch (command) {
        case XE2_CX0_M2P_COMMAND_WRITE_UNCOMMITTED:
            lane->regs[address & 0xFFF] = payload;
            xe2_cx0_sram_write(lane, address, payload);
            break;
        case XE2_CX0_M2P_COMMAND_WRITE_COMMITTED:
            lane->regs[address & 0xFFF] = payload;
            xe2_cx0_sram_write(lane, address, payload);
            lane->p2m = XE2_REG_CX0_P2M_RESPONSE_READY_MASK
                      | xe2_reg_field_prep(XE2_REG_CX0_P2M_COMMAND_TYPE_MASK, XE2_CX0_P2M_COMMAND_WRITE_ACK);
            break;
        case XE2_CX0_M2P_COMMAND_READ:
            lane->p2m = XE2_REG_CX0_P2M_RESPONSE_READY_MASK
                      | xe2_reg_field_prep(XE2_REG_CX0_P2M_COMMAND_TYPE_MASK, XE2_CX0_P2M_COMMAND_READ_ACK)
                      | xe2_reg_field_prep(XE2_REG_CX0_P2M_DATA_MASK, xe2_cx0_reg_read(lane, address));
            break;
        default:
            break;
    }
}

// -----------------------------------------------------------
// GuC controller (MMIO/CTB)
// -----------------------------------------------------------



static inline uint32_t xe2_guc_action_self_cfg(xe2_dev_t* xe2, uint32_t* actions)
{
    uint32_t key = xe2_reg_field_get(xe2_reg_genmask(31, 16), actions[1]);

    uint32_t response = xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_ORIGIN_MASK, 1) // Origin GUC
                      | xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_TYPE_MASK, 7)   // Success
                      | xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_DATA_MASK, 1);  // AUX data

    rvvm_addr_t value = xe2_concat_lohi(actions[2], actions[3]);

    switch (key) {
        case XE2_GUC_KLV_SELF_CFG_MEMIRQ_STATUS_ADDR_KEY:
            xe2->guc.memirq_status_addr = xe2_ggtt_translate(xe2, value);
            break;

        case XE2_GUC_KLV_SELF_CFG_MEMIRQ_SOURCE_ADDR_KEY:
            xe2->guc.memirq_source_addr = xe2_ggtt_translate(xe2, value);
            break;

        case XE2_GUC_KLV_SELF_CFG_H2G_CTB_ADDR_KEY:
            xe2->guc.ctb_h2g_addr = xe2_ggtt_translate(xe2, value);
            break;

        case XE2_GUC_KLV_SELF_CFG_H2G_CTB_SIZE_KEY:
            xe2->guc.ctb_h2g_size = value;
            break;

        case XE2_GUC_KLV_SELF_CFG_G2H_CTB_SIZE_KEY:
            xe2->guc.ctb_g2h_size = value;
            break;

        case XE2_GUC_KLV_SELF_CFG_H2G_CTB_DESCRIPTOR_ADDR_KEY:
            xe2->guc.ctb_h2g_descriptor_addr = xe2_ggtt_translate(xe2, value);
            break;

        case XE2_GUC_KLV_SELF_CFG_G2H_CTB_ADDR_KEY:
            xe2->guc.ctb_g2h_addr = xe2_ggtt_translate(xe2, value);
            break;

        case XE2_GUC_KLV_SELF_CFG_G2H_CTB_DESCRIPTOR_ADDR_KEY:
            xe2->guc.ctb_g2h_descriptor_addr = xe2_ggtt_translate(xe2, value);
            break;

        default:
            break;
    }

    return response;
}

// Emit the hwconfig table as key/length/value triplets into the supplied buffer
// and return its size in bytes. The driver first queries the size (address 0),
// then re-requests with a buffer to copy into.
static uint32_t xe2_guc_emit_hwconfig(xe2_dev_t* xe2, rvvm_addr_t ggtt_addr)
{
    static const uint32_t table[] = {
        XE2_HWCONFIG_ATTR_MAX_SLICES,    1, XE2_HWCONFIG_MAX_SLICES_VAL,
        XE2_HWCONFIG_ATTR_MAX_SUBSLICES, 1, XE2_HWCONFIG_MAX_SUBSLICES_VAL,
    };

    if (ggtt_addr != 0ULL) {
        xe2_dma_addr_t dst = xe2_ggtt_translate(xe2, ggtt_addr);
        for (size_t i = 0; i < STATIC_ARRAY_SIZE(table); i++) {
            xe2_dma_write_32(xe2, dst, i * 4, table[i]);
        }
    }

    return sizeof(table);
}

// This kind of GuC communication used mainly to bootstrap GuC CT
// channel.
//
// Commands pipeline:
//   write (GUC FW SW 1): 32 bit header
//   write (GUC FW SW 2): 32 bit payload
//   write (GUC FW SW 3): 32 bit payload
//   write (GUC FW SW 4): 32 bit payload
static inline void xe2_guc_action(xe2_dev_t* xe2, uint32_t* h2g, uint32_t* g2h)
{
    uint32_t arg = 0U;

    // GuC reports addresses of CTB, CTB descriptor (for both directions)
    // rvvm_info("GUC action (request):      %08x", h2g[0]);
    // rvvm_info("GUC action (key | len):    %08x", h2g[1]);
    // rvvm_info("GUC action (value hi):     %08x", h2g[2]);
    // rvvm_info("GUC action (value lo):     %08x", h2g[3]);

    switch (h2g[0]) {
        case XE2_GUC_ACTION_GET_HWCONFIG: {
            rvvm_addr_t ggtt = xe2_concat_lohi(h2g[1], h2g[2]);
            arg              = xe2_guc_emit_hwconfig(xe2, ggtt);
            break;
        }
        case XE2_GUC_ACTION_HOST2GUC_SELF_CFG: {
            arg = xe2_guc_action_self_cfg(xe2, h2g);
            break;
        }
        case XE2_GUC_ACTION_HOST2GUC_CONTROL_CTB:
            arg = 0; // GUC_CTB_CONTROL_ENABLE
            break;
        case XE2_GUC_ACTION_OPT_IN_FEATURE_KLV:
            arg = 1;
            break;
        default:
            break;
    }

    uint32_t cmd = xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_ORIGIN_MASK, 1) // Origin GUC
                 | xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_TYPE_MASK, 7)   // Success
                 | xe2_reg_field_prep(XE2_REG_GUC_FW_SW_X_MSG_0_DATA_MASK, arg);

    g2h[0] = cmd;
}

// Read/write one dword in a CTB ring; head/tail are dword indices that wrap
// within the buffer.
static inline uint32_t xe2_ctb_get(xe2_dev_t* xe2, xe2_dma_addr_t buf, uint32_t idx, uint32_t dwords)
{
    return xe2_dma_read_32(xe2, buf, (idx % dwords) * 4);
}

static inline void xe2_ctb_put(xe2_dev_t* xe2, xe2_dma_addr_t buf, uint32_t idx, uint32_t dwords, uint32_t val)
{
    xe2_dma_write_32(xe2, buf, (idx % dwords) * 4, val);
}

// Frame an HXG message into the G2H ring as [CTB header][n payload dwords],
// advance the producer tail and raise the GuC interrupt. 'fence' echoes the
// originating request so a blocking transport send completes.
static void xe2_guc_g2h_push(xe2_dev_t* xe2, uint32_t fence, const uint32_t* hxg, uint32_t n)
{
    uint32_t dwords = xe2->guc.ctb_g2h_size / 4;
    if (dwords == 0) {
        return;
    }

    uint32_t tail = xe2_dma_read_32(xe2, xe2->guc.ctb_g2h_descriptor_addr, 4);

    uint32_t header = xe2_reg_field_prep(XE2_GUC_CTB_MSG_0_FENCE, fence)
                    | xe2_reg_field_prep(XE2_GUC_CTB_MSG_0_FORMAT, XE2_GUC_CTB_FORMAT_HXG)
                    | xe2_reg_field_prep(XE2_GUC_CTB_MSG_0_NUM_DWORDS, n);

    xe2_ctb_put(xe2, xe2->guc.ctb_g2h_addr, tail, dwords, header);
    for (uint32_t i = 0; i < n; i++) {
        xe2_ctb_put(xe2, xe2->guc.ctb_g2h_addr, tail + XE2_GUC_CTB_HDR_LEN + i, dwords, hxg[i]);
    }

    tail = (tail + XE2_GUC_CTB_HDR_LEN + n) % dwords;
    xe2_dma_write_32(xe2, xe2->guc.ctb_g2h_descriptor_addr, 4, tail);

    // Deliver the GuC-to-host interrupt. The driver wires MSI-X vector 0 to the
    // register-based top-level handler (not the memirq path used for engines),
    // so a bare MSI is ignored unless the interrupt-identity register chain
    // reports a pending GuC source. Latch it and raise vector 0; without this
    // every blocking CT request (e.g. GuC opt-in) times out.
    xe2->guc.irq_pending = true;
    rvvm_pci_send_irq(xe2->pci_func, 0);
}

// Reply to a transport request with a single-dword success response.
static void xe2_guc_g2h_response(xe2_dev_t* xe2, uint32_t fence, uint32_t data0)
{
    uint32_t hxg = xe2_reg_field_prep(XE2_GUC_HXG_MSG_0_ORIGIN, XE2_GUC_HXG_ORIGIN_GUC)
                 | xe2_reg_field_prep(XE2_GUC_HXG_MSG_0_TYPE, XE2_GUC_HXG_TYPE_RESPONSE_SUCCESS)
                 | xe2_reg_field_prep(XE2_GUC_HXG_RESPONSE_MSG_0_DATA0, data0);
    xe2_guc_g2h_push(xe2, fence, &hxg, 1);
}

// Post an asynchronous G2H EVENT (no fence matching): an HXG header carrying the
// action in bits 15:0, followed by the event payload dwords. Used for the
// scheduling/deregister "done" notifications the driver blocks on.
static void xe2_guc_g2h_event(xe2_dev_t* xe2, uint32_t action, const uint32_t* payload, uint32_t n)
{
    uint32_t hxg[16];
    hxg[0] = xe2_reg_field_prep(XE2_GUC_HXG_MSG_0_ORIGIN, XE2_GUC_HXG_ORIGIN_GUC)
           | xe2_reg_field_prep(XE2_GUC_HXG_MSG_0_TYPE, XE2_GUC_HXG_TYPE_EVENT)
           | xe2_reg_field_prep(XE2_GUC_HXG_MSG_0_ACTION, action);
    for (uint32_t i = 0; i < n && i < 15; i++) {
        hxg[1 + i] = payload[i];
    }
    xe2_guc_g2h_push(xe2, 0, hxg, n + 1);
}

// -----------------------------------------------------------
// Intel BRW assembly -> SPIR-V translation
// -----------------------------------------------------------



// Hardware 7-bit opcodes from [6:0] of each instruction.
// Opcodes GFX12+/Xe2-compliant.
#define XE2_BRW_OP_ILLEGAL                 0
#define XE2_BRW_OP_SYNC                    1
#define XE2_BRW_OP_MOV                     97
#define XE2_BRW_OP_SEL                     98
#define XE2_BRW_OP_MOVI                    99
#define XE2_BRW_OP_NOT                     100
#define XE2_BRW_OP_AND                     101
#define XE2_BRW_OP_OR                      102
#define XE2_BRW_OP_XOR                     103
#define XE2_BRW_OP_SHR                     104
#define XE2_BRW_OP_SHL                     105
#define XE2_BRW_OP_SMOV                    106
#define XE2_BRW_OP_BFN                     107
#define XE2_BRW_OP_ASR                     108
#define XE2_BRW_OP_ROR                     110
#define XE2_BRW_OP_ROL                     111
#define XE2_BRW_OP_CMP                     112
#define XE2_BRW_OP_CMPN                    113
#define XE2_BRW_OP_CSEL                    114
#define XE2_BRW_OP_BFREV                   119
#define XE2_BRW_OP_BFE                     120
#define XE2_BRW_OP_BFI1                    121
#define XE2_BRW_OP_BFI2                    122
#define XE2_BRW_OP_JMPI                    32
#define XE2_BRW_OP_BRD                     33
#define XE2_BRW_OP_IF                      34
#define XE2_BRW_OP_BRC                     35
#define XE2_BRW_OP_ELSE                    36
#define XE2_BRW_OP_ENDIF                   37
#define XE2_BRW_OP_WHILE                   39
#define XE2_BRW_OP_BREAK                   40
#define XE2_BRW_OP_CONT                    41
#define XE2_BRW_OP_HALT                    42
#define XE2_BRW_OP_CALLA                   43
#define XE2_BRW_OP_CALL                    44
#define XE2_BRW_OP_RET                     45
#define XE2_BRW_OP_GOTO                    46
#define XE2_BRW_OP_JOIN                    47
#define XE2_BRW_OP_SEND                    49
#define XE2_BRW_OP_SENDC                   50
#define XE2_BRW_OP_SENDS                   51
#define XE2_BRW_OP_SENDSC                  52
#define XE2_BRW_OP_MATH                    56
#define XE2_BRW_OP_ADD                     64
#define XE2_BRW_OP_MUL                     65
#define XE2_BRW_OP_AVG                     66
#define XE2_BRW_OP_FRC                     67
#define XE2_BRW_OP_RNDU                    68
#define XE2_BRW_OP_RNDD                    69
#define XE2_BRW_OP_RNDE                    70
#define XE2_BRW_OP_RNDZ                    71
#define XE2_BRW_OP_MAC                     72
#define XE2_BRW_OP_MACH                    73
#define XE2_BRW_OP_LZD                     74
#define XE2_BRW_OP_FBH                     75
#define XE2_BRW_OP_FBL                     76
#define XE2_BRW_OP_CBIT                    77
#define XE2_BRW_OP_ADDC                    78
#define XE2_BRW_OP_SUBB                    79
#define XE2_BRW_OP_ADD3                    82
#define XE2_BRW_OP_MACL                    83
#define XE2_BRW_OP_SRND                    84
#define XE2_BRW_OP_DP4A                    88
#define XE2_BRW_OP_DPAS                    89
#define XE2_BRW_OP_MAD                     91
#define XE2_BRW_OP_MADM                    93
#define XE2_BRW_OP_NOP                     96

#define XE2_BRW_HW_OPCODE_HI               6
#define XE2_BRW_HW_OPCODE_LO               0
#define XE2_BRW_CMPT_CONTROL_BIT           29

// Generic (0/1/2-source) native instructions fields.
#define XE2_BRW_EXEC_SIZE_HI               20
#define XE2_BRW_EXEC_SIZE_LO               18
#define XE2_BRW_PRED_INV_BIT               28
#define XE2_BRW_PRED_CONTROL_HI            27
#define XE2_BRW_PRED_CONTROL_LO            26
#define XE2_BRW_QTR_CONTROL_HI             25
#define XE2_BRW_QTR_CONTROL_LO             24
#define XE2_BRW_MASK_CONTROL_BIT           31
#define XE2_BRW_SATURATE_BIT               34
#define XE2_BRW_DEBUG_CONTROL_BIT          30
#define XE2_BRW_COND_MODIFIER_HI           95
#define XE2_BRW_COND_MODIFIER_LO           92
#define XE2_BRW_SWSB_HI                    17
#define XE2_BRW_SWSB_LO                    8
#define XE2_BRW_FLAG_SUBREG_BIT            21
#define XE2_BRW_FLAG_REG_HI                23
#define XE2_BRW_FLAG_REG_LO                22
#define XE2_BRW_DST_REG_FILE_BIT           50
#define XE2_BRW_DST_HWTYPE_HI              39
#define XE2_BRW_DST_HWTYPE_LO              36
#define XE2_BRW_DST_REG_NR_HI              63
#define XE2_BRW_DST_REG_NR_LO              56
#define XE2_BRW_DST_SUBREG_HI              55
#define XE2_BRW_DST_SUBREG_LO              51
#define XE2_BRW_DST_SUBREG_LSB_BIT         33
#define XE2_BRW_DST_HSTRIDE_HI             49
#define XE2_BRW_DST_HSTRIDE_LO             48
#define XE2_BRW_DST_ADDRESS_MODE_BIT       35

#define XE2_BRW_SRC0_REG_FILE_BIT          66
#define XE2_BRW_SRC0_IS_IMM_BIT            46
#define XE2_BRW_SRC0_HWTYPE_HI             43
#define XE2_BRW_SRC0_HWTYPE_LO             40
#define XE2_BRW_SRC0_REG_NR_HI             79
#define XE2_BRW_SRC0_REG_NR_LO             72
#define XE2_BRW_SRC0_DA1_SUBREG_HI         71
#define XE2_BRW_SRC0_DA1_SUBREG_LO         67
#define XE2_BRW_SRC0_IA_SUBREG_HI          79
#define XE2_BRW_SRC0_IA_SUBREG_LO          76
#define XE2_BRW_SRC0_SUBREG_LSB_BIT        87
#define XE2_BRW_SRC0_VSTRIDE_HI            86
#define XE2_BRW_SRC0_VSTRIDE_LO            84
#define XE2_BRW_SRC0_WIDTH_HI              83
#define XE2_BRW_SRC0_WIDTH_LO              81
#define XE2_BRW_SRC0_HSTRIDE_HI            65
#define XE2_BRW_SRC0_HSTRIDE_LO            64
#define XE2_BRW_SRC0_ADDRESS_MODE_BIT      80
#define XE2_BRW_SRC0_NEGATE_BIT            45
#define XE2_BRW_SRC0_ABS_BIT               44

#define XE2_BRW_SRC1_IS_IMM_BIT            47
#define XE2_BRW_SRC1_HWTYPE_HI             91
#define XE2_BRW_SRC1_HWTYPE_LO             88
#define XE2_BRW_SRC1_DA_REG_NR_HI          111
#define XE2_BRW_SRC1_DA_REG_NR_LO          104
#define XE2_BRW_SRC1_DA1_SUBREG_HI         103
#define XE2_BRW_SRC1_DA1_SUBREG_LO         99
#define XE2_BRW_SRC1_IA_SUBREG_HI          111
#define XE2_BRW_SRC1_IA_SUBREG_LO          108
#define XE2_BRW_SRC1_HSTRIDE_HI            97
#define XE2_BRW_SRC1_HSTRIDE_LO            96
#define XE2_BRW_SRC1_NEGATE_BIT            121
#define XE2_BRW_SRC1_ABS_BIT               120
#define XE2_BRW_SRC1_VSTRIDE_HI            118
#define XE2_BRW_SRC1_VSTRIDE_LO            116
#define XE2_BRW_SRC1_WIDTH_HI              115
#define XE2_BRW_SRC1_WIDTH_LO              113
#define XE2_BRW_SRC1_ADDRESS_MODE_BIT      112

// 3-source (align1, "mad"-style) native instruction fields.
#define XE2_BRW_A3_EXEC_TYPE_BIT           39
#define XE2_BRW_A3_DST_REG_FILE_BIT        50
#define XE2_BRW_A3_DST_HWTYPE_HI           38
#define XE2_BRW_A3_DST_HWTYPE_LO           36
#define XE2_BRW_A3_DST_REG_NR_HI           63
#define XE2_BRW_A3_DST_REG_NR_LO           56
#define XE2_BRW_A3_DST_SUBREG_HI           55
#define XE2_BRW_A3_DST_SUBREG_LO           51
#define XE2_BRW_A3_DST_HSTRIDE_BIT         48

#define XE2_BRW_A3_SRC0_REG_FILE_BIT       66
#define XE2_BRW_A3_SRC0_IS_IMM_BIT         46
#define XE2_BRW_A3_SRC0_HWTYPE_HI          42
#define XE2_BRW_A3_SRC0_HWTYPE_LO          40
#define XE2_BRW_A3_SRC0_REG_NR_HI          79
#define XE2_BRW_A3_SRC0_REG_NR_LO          72
#define XE2_BRW_A3_SRC0_SUBREG_HI          71
#define XE2_BRW_A3_SRC0_SUBREG_LO          67
#define XE2_BRW_A3_SRC0_HSTRIDE_HI         65
#define XE2_BRW_A3_SRC0_HSTRIDE_LO         64
#define XE2_BRW_A3_SRC0_NEGATE_BIT         45
#define XE2_BRW_A3_SRC0_ABS_BIT            44

#define XE2_BRW_A3_SRC1_REG_NR_HI          111
#define XE2_BRW_A3_SRC1_REG_NR_LO          104
#define XE2_BRW_A3_SRC1_SUBREG_HI          103
#define XE2_BRW_A3_SRC1_SUBREG_LO          99
#define XE2_BRW_A3_SRC1_REG_FILE_BIT       98
#define XE2_BRW_A3_SRC1_HSTRIDE_HI         97
#define XE2_BRW_A3_SRC1_HSTRIDE_LO         96
#define XE2_BRW_A3_SRC1_HWTYPE_HI          90
#define XE2_BRW_A3_SRC1_HWTYPE_LO          88
#define XE2_BRW_A3_SRC1_NEGATE_BIT         87
#define XE2_BRW_A3_SRC1_ABS_BIT            86

#define XE2_BRW_A3_SRC2_REG_NR_HI          127
#define XE2_BRW_A3_SRC2_REG_NR_LO          120
#define XE2_BRW_A3_SRC2_SUBREG_HI          119
#define XE2_BRW_A3_SRC2_SUBREG_LO          115
#define XE2_BRW_A3_SRC2_REG_FILE_BIT       114
#define XE2_BRW_A3_SRC2_HSTRIDE_HI         113
#define XE2_BRW_A3_SRC2_HSTRIDE_LO         112
#define XE2_BRW_A3_SRC2_NEGATE_BIT         85
#define XE2_BRW_A3_SRC2_ABS_BIT            84
#define XE2_BRW_A3_SRC2_HWTYPE_HI          82
#define XE2_BRW_A3_SRC2_HWTYPE_LO          80

// SEND/SENDC native instruction fields.
#define XE2_BRW_SEND_SRC1_REG_NR_HI        111
#define XE2_BRW_SEND_SRC1_REG_NR_LO        104
#define XE2_BRW_SEND_SRC1_LEN_HI           103
#define XE2_BRW_SEND_SRC1_LEN_LO           99
#define XE2_BRW_SEND_SRC1_REG_FILE_BIT     98
#define XE2_BRW_SEND_SFID_HI               95
#define XE2_BRW_SEND_SFID_LO               92
#define XE2_BRW_SEND_EX_DESC_IA_SUBREG_HI  42
#define XE2_BRW_SEND_EX_DESC_IA_SUBREG_LO  40
#define XE2_BRW_SEND_EX_BSO_BIT            39
#define XE2_BRW_SEND_SEL_REG32_DESC_BIT    48
#define XE2_BRW_SEND_SEL_REG32_EX_DESC_BIT 49
#define XE2_BRW_SEND_DST_REG_FILE_BIT      50
#define XE2_BRW_SEND_EOT_BIT               34

// Compacted (8-byte) instructions
#define XE2_BRW_C_CONTROL_INDEX_HI         22
#define XE2_BRW_C_CONTROL_INDEX_LO         18
#define XE2_BRW_C_DATATYPE_LO_HI           28
#define XE2_BRW_C_DATATYPE_LO_LO           26
#define XE2_BRW_C_DATATYPE_HI_HI           31
#define XE2_BRW_C_DATATYPE_HI_LO           30
#define XE2_BRW_C_SUBREG_INDEX_HI          51
#define XE2_BRW_C_SUBREG_INDEX_LO          48
#define XE2_BRW_C_SRC0_INDEX_HI            25
#define XE2_BRW_C_SRC0_INDEX_LO            23
#define XE2_BRW_C_SRC1_INDEX_HI            55
#define XE2_BRW_C_SRC1_INDEX_LO            52
#define XE2_BRW_C_DST_REG_NR_HI            39
#define XE2_BRW_C_DST_REG_NR_LO            32
#define XE2_BRW_C_DST_HSTRIDE_HI           49
#define XE2_BRW_C_DST_HSTRIDE_LO           48
#define XE2_BRW_C_SRC0_REG_NR_HI           47
#define XE2_BRW_C_SRC0_REG_NR_LO           40
#define XE2_BRW_C_SRC1_REG_NR_HI           63
#define XE2_BRW_C_SRC1_REG_NR_LO           56
#define XE2_BRW_C_SWSB_HI                  17
#define XE2_BRW_C_SWSB_LO                  8
#define XE2_BRW_C_DEBUG_CONTROL_BIT        7

// Compacted (8-byte) instruction, 3-source form.
#define XE2_BRW_C3_DST_REG_NR_HI           39
#define XE2_BRW_C3_DST_REG_NR_LO           32
#define XE2_BRW_C3_SRC_INDEX_HI            25
#define XE2_BRW_C3_SRC_INDEX_LO            22
#define XE2_BRW_C3_CONTROL_INDEX_HI        21
#define XE2_BRW_C3_CONTROL_INDEX_LO        18
#define XE2_BRW_C3_SWSB_HI                 17
#define XE2_BRW_C3_SWSB_LO                 8

typedef struct {
    uint64_t lo;
    uint64_t hi;
} xe2_qword_t;

// Pull bits [hi:lo] (inclusive) out of a 128-bit instruction word.
static inline uint64_t xe2_brw_mask(const xe2_qword_t* w, int mask_lo, int mask_hi)
{
    if (mask_hi < 0 || mask_lo < 0 || mask_hi < mask_lo) {
        return 0;
    }

    int width = mask_hi - mask_lo + 1;
    if (width > 64) {
        width = 64;
    }

    uint64_t mask = (width == 64) ? ~0ULL : ((1ULL << width) - 1);

    if (mask_lo >= 64) {
        return (w->hi >> (mask_lo - 64)) & mask;
    }
    if (mask_hi < 64) {
        return (w->lo >> mask_lo) & mask;
    }

    uint64_t lo = w->lo >> mask_lo;
    uint64_t hi = w->hi << (64 - mask_lo);
    return (lo | hi) & mask;
}

static int xe2_brw_op_nsrc(unsigned op)
{
    switch (op) {
        case XE2_BRW_OP_MOV:
        case XE2_BRW_OP_NOT:
        case XE2_BRW_OP_FRC:
        case XE2_BRW_OP_RNDU:
        case XE2_BRW_OP_RNDD:
        case XE2_BRW_OP_RNDE:
        case XE2_BRW_OP_RNDZ:
        case XE2_BRW_OP_LZD:
        case XE2_BRW_OP_FBH:
        case XE2_BRW_OP_FBL:
        case XE2_BRW_OP_CBIT:
        case XE2_BRW_OP_BFREV:
        case XE2_BRW_OP_SYNC:
            return 1;
        case XE2_BRW_OP_SEL:
        case XE2_BRW_OP_AND:
        case XE2_BRW_OP_OR:
        case XE2_BRW_OP_XOR:
        case XE2_BRW_OP_SHR:
        case XE2_BRW_OP_SHL:
        case XE2_BRW_OP_ASR:
        case XE2_BRW_OP_ROR:
        case XE2_BRW_OP_ROL:
        case XE2_BRW_OP_CMP:
        case XE2_BRW_OP_CMPN:
        case XE2_BRW_OP_BFI1:
        case XE2_BRW_OP_MATH:
        case XE2_BRW_OP_ADD:
        case XE2_BRW_OP_MUL:
        case XE2_BRW_OP_AVG:
        case XE2_BRW_OP_MAC:
        case XE2_BRW_OP_MACH:
        case XE2_BRW_OP_ADDC:
        case XE2_BRW_OP_SUBB:
        case XE2_BRW_OP_MACL:
        case XE2_BRW_OP_SRND:
        case XE2_BRW_OP_SEND:
        case XE2_BRW_OP_SENDC:
            return 2;
        case XE2_BRW_OP_BFN:
        case XE2_BRW_OP_CSEL:
        case XE2_BRW_OP_BFE:
        case XE2_BRW_OP_BFI2:
        case XE2_BRW_OP_ADD3:
        case XE2_BRW_OP_DP4A:
        case XE2_BRW_OP_DPAS:
        case XE2_BRW_OP_MAD:
        case XE2_BRW_OP_MADM:
            return 3;
        default:
            return 0;
    }
}

static inline bool xe2_brw_op_send(uint32_t op)
{
    return op == XE2_BRW_OP_SEND || op == XE2_BRW_OP_SENDC;
}

static inline bool xe2_brw_op_3_src(uint32_t op)
{
    return xe2_brw_op_nsrc(op) == 3 && !xe2_brw_op_send(op);
}

static inline uint32_t xe2_brw_op_type_size(uint32_t hw_type)
{
    // 0 -> 1
    // 1 -> 2
    // 2 -> 4
    // 3 -> 8
    return 1U << (hw_type & 0x3);
}

static const char* xe2_brw_op_name(unsigned op)
{
    switch (op) {
        case XE2_BRW_OP_ILLEGAL:
            return "illegal";
        case XE2_BRW_OP_SYNC:
            return "sync";
        case XE2_BRW_OP_MOV:
            return "mov";
        case XE2_BRW_OP_SEL:
            return "sel";
        case XE2_BRW_OP_MOVI:
            return "movi";
        case XE2_BRW_OP_NOT:
            return "not";
        case XE2_BRW_OP_AND:
            return "and";
        case XE2_BRW_OP_OR:
            return "or";
        case XE2_BRW_OP_XOR:
            return "xor";
        case XE2_BRW_OP_BFN:
            return "bfn";
        case XE2_BRW_OP_SHR:
            return "shr";
        case XE2_BRW_OP_SHL:
            return "shl";
        case XE2_BRW_OP_SMOV:
            return "smov";
        case XE2_BRW_OP_ASR:
            return "asr";
        case XE2_BRW_OP_ROR:
            return "ror";
        case XE2_BRW_OP_ROL:
            return "rol";
        case XE2_BRW_OP_CMP:
            return "cmp";
        case XE2_BRW_OP_CMPN:
            return "cmpn";
        case XE2_BRW_OP_CSEL:
            return "csel";
        case XE2_BRW_OP_BFREV:
            return "bfrev";
        case XE2_BRW_OP_BFE:
            return "bfe";
        case XE2_BRW_OP_BFI1:
            return "bfi1";
        case XE2_BRW_OP_BFI2:
            return "bfi2";
        case XE2_BRW_OP_JMPI:
            return "jmpi";
        case XE2_BRW_OP_BRD:
            return "brd";
        case XE2_BRW_OP_IF:
            return "if";
        case XE2_BRW_OP_BRC:
            return "brc";
        case XE2_BRW_OP_ELSE:
            return "else";
        case XE2_BRW_OP_ENDIF:
            return "endif";
        case XE2_BRW_OP_WHILE:
            return "while";
        case XE2_BRW_OP_BREAK:
            return "break";
        case XE2_BRW_OP_CONT:
            return "cont";
        case XE2_BRW_OP_HALT:
            return "halt";
        case XE2_BRW_OP_CALLA:
            return "calla";
        case XE2_BRW_OP_CALL:
            return "call";
        case XE2_BRW_OP_RET:
            return "ret";
        case XE2_BRW_OP_GOTO:
            return "goto";
        case XE2_BRW_OP_JOIN:
            return "join";
        case XE2_BRW_OP_SEND:
            return "send";
        case XE2_BRW_OP_SENDC:
            return "sendc";
        case XE2_BRW_OP_MATH:
            return "math";
        case XE2_BRW_OP_ADD:
            return "add";
        case XE2_BRW_OP_MUL:
            return "mul";
        case XE2_BRW_OP_AVG:
            return "avg";
        case XE2_BRW_OP_FRC:
            return "frc";
        case XE2_BRW_OP_RNDU:
            return "rndu";
        case XE2_BRW_OP_RNDD:
            return "rndd";
        case XE2_BRW_OP_RNDE:
            return "rnde";
        case XE2_BRW_OP_RNDZ:
            return "rndz";
        case XE2_BRW_OP_MAC:
            return "mac";
        case XE2_BRW_OP_MACH:
            return "mach";
        case XE2_BRW_OP_LZD:
            return "lzd";
        case XE2_BRW_OP_FBH:
            return "fbh";
        case XE2_BRW_OP_FBL:
            return "fbl";
        case XE2_BRW_OP_CBIT:
            return "cbit";
        case XE2_BRW_OP_ADDC:
            return "addc";
        case XE2_BRW_OP_SUBB:
            return "subb";
        case XE2_BRW_OP_ADD3:
            return "add3";
        case XE2_BRW_OP_MACL:
            return "macl";
        case XE2_BRW_OP_SRND:
            return "srnd";
        case XE2_BRW_OP_DP4A:
            return "dp4a";
        case XE2_BRW_OP_DPAS:
            return "dpas";
        case XE2_BRW_OP_MAD:
            return "mad";
        case XE2_BRW_OP_MADM:
            return "madm";
        case XE2_BRW_OP_NOP:
            return "nop";
        default:
            return "???";
    }
}

// Instruction operand expansion tables. Compact
// EU instructions does not contain full payload regioning fields,
// instead, they contain small indicies to these tables. Values in
// these tables has exact format of uncompacted 128-bit instruction.
static const uint16_t xe2_subreg_table[16] = {
    0b000000000000, // .0 .0
    0b000010000000, // .0 .4
    0b000000000100, // .4 .0
    0b010000000000, // .0 .32
    0b001000000000, // .0 .16
    0b000000001000, // .8 .0
    0b000100000000, // .0 .8
    0b010100000000, // .0 .40
    0b011000000000, // .0 .48
    0b000110000000, // .0 .12
    0b000000010000, // .16 .0
    0b011010000000, // .0 .52
    0b001100000000, // .0 .24
    0b011100000000, // .0 .56
    0b010110000000, // .0 .44
    0b010010000000, // .0 .36
};

static const uint16_t xe2_src0_index_table[8] = {
    0b00100000000, //  r<1;1,0>
    0b00000000000, //  r<0;1,0>
    0b01000000000, //  r<2;1,0>
    0b00100000010, // -r<1;1,0>
    0b01100000000, //  r<4;1,0>
    0b00100000001, //  (abs)r<1;1,0>
    0b00000000010, // -r<0;1,0>
    0b01001000000, //  r<2;4,0>
};

static const uint16_t xe2_src1_index_table[16] = {
    0b0000100000000000, //  r<1;1,0>.0
    0b0000000000000000, //  r<0;1,0>.0
    0b1000100000000000, // -r<1;1,0>.0
    0b0000000000010000, //  r<0;1,0>.8
    0b0000000000001000, //  r<0;1,0>.4
    0b0000000000011000, //  r<0;1,0>.12
    0b0000000001010000, //  r<0;1,0>.40
    0b0000000001000000, //  r<0;1,0>.32
    0b0000000000100000, //  r<0;1,0>.16
    0b0000000001111000, //  r<0;1,0>.60
    0b0000000000111000, //  r<0;1,0>.28
    0b0000000000101000, //  r<0;1,0>.20
    0b0000000001011000, //  r<0;1,0>.44
    0b0000000001001000, //  r<0;1,0>.36
    0b0000000001110000, //  r<0;1,0>.56
    0b0000000000110000, //  r<0;1,0>.24
};

static const uint32_t xe2_brw_hstride_decode[4] = {0, 1, 2, 4};

static const uint32_t xe2_brw_width_decode[8] = {1, 2, 4, 8, 16, 32, 0, 0};

static const uint32_t xe2_brw_vstride_decode[8] = {0, 1, 2, 4, 8, 16, 32, 64};

// Decoded operand used by the emitter (register number + modifiers).
typedef struct {
    uint32_t reg;
    uint32_t subreg;
    bool     neg;
    bool     abs;
    bool     is_imm;
    bool     mode;
    uint32_t imm_u32;
    uint8_t  hstride;
    uint8_t  vstride;
    uint8_t  width;
} xe2_brw_operand_t;

static forceinline uint32_t xe2_brw_parse_dst(const xe2_qword_t* qw)
{
    return xe2_brw_mask(qw, XE2_BRW_DST_REG_NR_LO, XE2_BRW_DST_REG_NR_HI);
}

// Module scaffolding for a guest kernel cross-compiled to SPIR-V: the
// register file, the pushed constant block, the stage's inputs/outputs
// and the entry point. The BRW instruction translation itself lives in
// gpu-xe2.c and drives this through the load/store helpers below.
//
// Register model. A GRF is one Function-local float. That is a heavy
// simplification of a 64-byte SIMD register, but it matches how the
// instruction translation treats operands today, and it keeps the
// constant binding below honest: what matters for constants is which
// (register, subregister) pair a read names, and that is modelled
// exactly.
//
// Constant binding. Xe2 dispatches a thread with the gathered constant
// buffers already resident in the GRFs that follow the fixed payload
// registers (see xe2_push_const_grf_base). A kernel therefore reads its
// uniforms as plain register reads. We recover that: a read of a
// register at or above the stage's push constant base that the kernel
// has not written yet is a read of pushed constant data, and compiles
// into a load from the uniform block. Registers the kernel wrote first
// are its own temporaries and stay Function-local, so scratch use of
// high registers keeps working.


// Descriptor set/binding the pushed constants of a stage are bound at.
// One set, one binding per stage, so a pipeline can carry the constants
// of every stage at once without them colliding.
#define XE2_SHADER_CONST_SET           0
#define XE2_SHADER_CONST_BINDING(kind) ((uint32_t)(kind))

static forceinline void xe2_spirv_add_iface(xe2_spirv_ctx_t* ctx, uint32_t var)
{
    // SPIR-V 1.3 entry points list Input and Output variables only;
    // globals in other storage classes joined the interface in 1.4.
    if (ctx->entry_iface_n < STATIC_ARRAY_SIZE(ctx->entry_iface)) {
        ctx->entry_iface[ctx->entry_iface_n++] = var;
    }
}

// Declares the stage's outputs. Inputs are not wired yet: the kernel
// reads its varyings out of the URB payload registers, which we do not
// model, so those reads resolve to undefined Function-locals.
static forceinline void xe2_spirv_declare_io(xe2_spirv_ctx_t* ctx)
{
    uint32_t out_v4 = spirv_type_ptr(&ctx->mod, SPIRV_STORAGE_CLASS_OUTPUT, ctx->v4ty);

    if (ctx->stage == XE2_SHADER_PS) {
        ctx->color_out = spirv_global_var(&ctx->mod, out_v4, SPIRV_STORAGE_CLASS_OUTPUT);
        spirv_decorate_1(&ctx->mod, ctx->color_out, SPIRV_DECORATION_LOCATION, 0);
        spirv_name(&ctx->mod, ctx->color_out, "out_color");
        xe2_spirv_add_iface(ctx, ctx->color_out);
    } else {
        ctx->position_out = spirv_global_var(&ctx->mod, out_v4, SPIRV_STORAGE_CLASS_OUTPUT);
        spirv_decorate_1(&ctx->mod, ctx->position_out, SPIRV_DECORATION_BUILTIN, SPIRV_BUILTIN_POSITION);
        spirv_name(&ctx->mod, ctx->position_out, "out_position");
        xe2_spirv_add_iface(ctx, ctx->position_out);
    }
}

// First GRF holding pushed constants, per stage. r0 is the dispatch
// header; the stage-specific payload registers follow it, and the
// gathered constants come after those. These are the defaults for a
// plain SIMD dispatch - they are copied into xe2_shader_stage_t at
// compile time so a stage can later derive its own base from the real
// dispatch state (3DSTATE_VS/PS payload fields) without touching the
// SPIR-V emitter.
//
//   VS  r0 header, r1 URB handles          -> r2
//   HS  r0 header, r1 URB handles, r2 ids  -> r3
//   DS  r0 header, r1 URB handles          -> r2
//   GS  r0 header, r1 URB handles          -> r2
//   PS  r0 header, r1 barycentric setup    -> r2
static inline uint32_t xe2_push_const_grf_base(xe2_shader_kind_t kind)
{
    switch (kind) {
        case XE2_SHADER_HS:
            return 3;
        case XE2_SHADER_CS:
            return 1;
        default:
            return 2;
    }
}

static inline const char* xe2_shader_kind_to_string(xe2_shader_kind_t kind)
{
    switch (kind) {
        case XE2_SHADER_VS:
            return "XE2_SHADER_VS";
        case XE2_SHADER_HS:
            return "XE2_SHADER_HS";
        case XE2_SHADER_DS:
            return "XE2_SHADER_DS";
        case XE2_SHADER_GS:
            return "XE2_SHADER_GS";
        case XE2_SHADER_PS:
            return "XE2_SHADER_PS";
        case XE2_SHADER_CS:
            return "XE2_SHADER_CS";
        default:
            rvvm_fatal("Unknown shader kind (xe2_shader_kind_t): %u", kind);
            return "";
    }
}

// Starts a module for one kernel. Everything the translation needs is
// live once this returns: base types, the constant block, the stage
// outputs and an open entry function.
static forceinline void xe2_spirv_begin(xe2_spirv_ctx_t* ctx, xe2_shader_kind_t stage)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->stage         = stage;
    ctx->push_grf_base = xe2_push_const_grf_base(stage);

    spirv_module_init(&ctx->mod);
    spirv_module_begin(&ctx->mod);

    ctx->void_ty = spirv_type_void(&ctx->mod);
    ctx->fn_ty   = spirv_type_func_void(&ctx->mod);
    ctx->fty     = spirv_type_float32(&ctx->mod);
    ctx->v4ty    = spirv_type_vec4_float32(&ctx->mod);

    ctx->const_var = spirv_uniform_vec4_array_block(&ctx->mod, XE2_CONST_MAX_VEC4, XE2_SHADER_CONST_SET,
                                                    XE2_SHADER_CONST_BINDING(stage), &ctx->const_elem_ptr);
    spirv_name(&ctx->mod, ctx->const_var, "xe2_constants");

    xe2_spirv_declare_io(ctx);

    ctx->func = spirv_func_begin(&ctx->mod, ctx->void_ty, ctx->fn_ty);
}

// Function-local backing store for a register, created on first use.
static forceinline uint32_t xe2_spirv_grf(xe2_spirv_ctx_t* ctx, uint32_t grf)
{
    if (grf >= XE2_SHADER_MAX_GRF) {
        grf = 0;
    }
    if (!ctx->grf_var[grf]) {
        uint32_t pty      = spirv_type_ptr(&ctx->mod, SPIRV_STORAGE_CLASS_FUNCTION, ctx->fty);
        ctx->grf_var[grf] = spirv_local_var(&ctx->mod, pty);
    }
    return ctx->grf_var[grf];
}

// Dword offset into the gathered payload that a (GRF, subregister) pair
// addresses. Only meaningful for registers at or above the stage's push
// constant base; the caller checks that and the XE2_CONST_MAX_DWORDS
// bound before using the result.
static inline uint32_t xe2_const_dword_index(uint32_t grf, uint32_t subreg, uint32_t push_grf_base)
{
    return (grf - push_grf_base) * XE2_GRF_DWORDS + subreg;
}

// True when a read of this register names pushed constant data rather
// than a value the kernel produced. See the binding note at the top.
static forceinline bool xe2_spirv_grf_is_const(const xe2_spirv_ctx_t* ctx, uint32_t grf, uint32_t subreg)
{
    if (grf >= XE2_SHADER_MAX_GRF || grf < ctx->push_grf_base || ctx->grf_written[grf]) {
        return false;
    }
    return xe2_const_dword_index(grf, subreg, ctx->push_grf_base) < XE2_CONST_MAX_DWORDS;
}

// Loads one dword of pushed constant data as a float. The block is a
// vec4 array, so the dword index splits into (element, component).
static forceinline uint32_t xe2_spirv_load_const(xe2_spirv_ctx_t* ctx, uint32_t grf, uint32_t subreg)
{
    uint32_t dword      = xe2_const_dword_index(grf, subreg, ctx->push_grf_base);
    uint32_t indices[3] = {
        spirv_type_const_uint32(&ctx->mod, 0), // Block member 0: the array.
        spirv_type_const_uint32(&ctx->mod, dword / 4),
        spirv_type_const_uint32(&ctx->mod, dword % 4),
    };
    uint32_t ptr
        = spirv_access_chain(&ctx->mod, ctx->const_elem_ptr, ctx->const_var, indices, STATIC_ARRAY_SIZE(indices));
    return spirv_op_load(&ctx->mod, ctx->fty, ptr);
}

static forceinline uint32_t xe2_spirv_load_grf(xe2_spirv_ctx_t* ctx, uint32_t grf, uint32_t subreg, bool neg, bool abs)
{
    uint32_t id = xe2_spirv_grf_is_const(ctx, grf, subreg)
                    ? xe2_spirv_load_const(ctx, grf, subreg)
                    : spirv_op_load(&ctx->mod, ctx->fty, xe2_spirv_grf(ctx, grf));
    if (abs) {
        id = spirv_ext_inst1(&ctx->mod, ctx->fty, SPIRV_GLSL_STD450_FABS, id);
    }
    if (neg) {
        id = spirv_op_fneg(&ctx->mod, ctx->fty, id);
    }
    return id;
}

static forceinline void xe2_spirv_store_grf(xe2_spirv_ctx_t* ctx, uint32_t grf, uint32_t val)
{
    spirv_op_store(&ctx->mod, xe2_spirv_grf(ctx, grf), val);
    if (grf < XE2_SHADER_MAX_GRF) {
        ctx->grf_written[grf] = true;
    }
}

// Reads four consecutive registers as a vec4. Used for the message
// payload a kernel hands to the URB write / render target write.
static forceinline uint32_t xe2_spirv_load_grf_vec4(xe2_spirv_ctx_t* ctx, uint32_t base, bool w_is_one)
{
    uint32_t x = xe2_spirv_load_grf(ctx, base + 0, 0, false, false);
    uint32_t y = xe2_spirv_load_grf(ctx, base + 1, 0, false, false);
    uint32_t z = xe2_spirv_load_grf(ctx, base + 2, 0, false, false);
    uint32_t w
        = w_is_one ? spirv_type_const_float32(&ctx->mod, 1.0f) : xe2_spirv_load_grf(ctx, base + 3, 0, false, false);
    return spirv_composite_construct4(&ctx->mod, ctx->v4ty, x, y, z, w);
}

// Translates the kernel's output message into a store to the stage
// output: a URB write becomes gl_Position, a render target write becomes
// the colour attachment.
static forceinline void xe2_spirv_emit_output(xe2_spirv_ctx_t* ctx, uint32_t payload_grf)
{
    if (ctx->stage == XE2_SHADER_PS && ctx->color_out) {
        spirv_op_store(&ctx->mod, ctx->color_out, xe2_spirv_load_grf_vec4(ctx, payload_grf, false));
        ctx->wrote_output = true;
    } else if (ctx->stage != XE2_SHADER_PS && ctx->position_out) {
        spirv_op_store(&ctx->mod, ctx->position_out, xe2_spirv_load_grf_vec4(ctx, payload_grf, true));
        ctx->wrote_output = true;
    }
}

// Closes the module and serializes it. A stage whose output message we
// failed to recognise would otherwise leave gl_Position or the colour
// attachment undefined, so give them a defined value instead - a black
// pixel or a degenerate vertex is debuggable, garbage is not.
static forceinline int xe2_spirv_finish(xe2_spirv_ctx_t* ctx, uint32_t** spirv, uint32_t* nwords)
{
    if (!ctx->wrote_output) {
        uint32_t zero = spirv_type_const_float32(&ctx->mod, 0.0f);
        uint32_t one  = spirv_type_const_float32(&ctx->mod, 1.0f);
        uint32_t def  = spirv_composite_construct4(&ctx->mod, ctx->v4ty, zero, zero, zero, one);
        spirv_op_store(&ctx->mod, ctx->stage == XE2_SHADER_PS ? ctx->color_out : ctx->position_out, def);
    }

    spirv_func_end(&ctx->mod);

    uint32_t exec_model = (ctx->stage == XE2_SHADER_PS) ? SPIRV_EXECUTION_MODEL_FRAGMENT : SPIRV_EXECUTION_MODEL_VERTEX;
    spirv_entry_point(&ctx->mod, exec_model, ctx->func, "main", ctx->entry_iface, ctx->entry_iface_n);
    if (ctx->stage == XE2_SHADER_PS) {
        spirv_exec_mode0(&ctx->mod, ctx->func, SPIRV_EXECUTION_MODE_ORIGIN_UPPER_LEFT);
    }

    int rc = spirv_module_finish(&ctx->mod, spirv, nwords);
    spirv_module_free(&ctx->mod);
    return rc;
}

static forceinline uint32_t xe2_spirv_load_operand(xe2_spirv_ctx_t* ctx, const xe2_brw_operand_t* op)
{
    if (op->is_imm) {
        float f;
        memcpy(&f, &op->imm_u32, 4);
        uint32_t id = spirv_type_const_float32(&ctx->mod, f);
        if (op->neg) {
            id = spirv_op_fneg(&ctx->mod, ctx->fty, id);
        }
        return id;
    }
    // The subregister matters here: it is what tells apart two uniforms
    // gathered into the same register.
    return xe2_spirv_load_grf(ctx, op->reg, op->subreg, op->neg, op->abs);
}

// No width field in align1 3-src. Width is architecturally fixed at 8;
// vstride is derived, not stored: 0 if broadcast (hstride==0), else hstride*width.
static uint32_t xe2_brw_3_src_vstride(uint32_t hstride)
{
    return hstride ? hstride * 8 : 0;
}

/* MATH function control sits in the low bits of the conditional modifier /
 * shared control field on Xe2; fall back to bits of dword0 when needed. */
static inline uint32_t xe2_brw_math_fc(const xe2_qword_t* qw)
{
    // Cond modifier [95:92] often carries FC for MATH.
    uint32_t fc = xe2_brw_mask(qw, 92, 95);
    if (fc == 0) {
        fc = (qw->lo >> 24) & 0xF;
    }
    return fc;
}

#define XE2_BRW_SFID_URB 6

// A send carries the thread's output message: the URB write that hands a
// vertex to the next stage, or the render target write that hands a
// fragment to the framebuffer. Either way its payload register holds the
// value we want.
//
// The fragment case keys off EOT rather than the SFID: a render target
// write is always the message a fragment thread ends on, whereas the
// dataport SFID it travels under has been renumbered across generations.
static forceinline void xe2_brw_emit_send(xe2_spirv_ctx_t* spirv_ctx, uint32_t sfid, bool eot,
                                          const xe2_brw_operand_t* s0, const xe2_brw_operand_t* s1)
{
    bool is_output = (spirv_ctx->stage == XE2_SHADER_PS) ? eot : (sfid == XE2_BRW_SFID_URB);
    if (!is_output) {
        return;
    }
    xe2_spirv_emit_output(spirv_ctx, s0->reg ? s0->reg : s1->reg);
}

static forceinline void xe2_brw_emit_spirv(xe2_spirv_ctx_t* spirv_ctx, const xe2_qword_t* qw, uint32_t op,
                                           const xe2_brw_operand_t* dst, const xe2_brw_operand_t* s0,
                                           const xe2_brw_operand_t* s1, const xe2_brw_operand_t* s2)
{
    switch (op) {
        case XE2_BRW_OP_MOV:
        case XE2_BRW_OP_SEL:
        case XE2_BRW_OP_FRC:
        case XE2_BRW_OP_RNDU:
        case XE2_BRW_OP_RNDD:
        case XE2_BRW_OP_RNDE:
        case XE2_BRW_OP_RNDZ: {
            uint32_t v = xe2_spirv_load_operand(spirv_ctx, s0);
            if (op == XE2_BRW_OP_FRC) {
                // frac(x) = x - floor(x); approximate with x for now.
            }
            xe2_spirv_store_grf(spirv_ctx, dst->reg, v);
            break;
        }
        case XE2_BRW_OP_ADD:
        case XE2_BRW_OP_AVG: {
            uint32_t a = xe2_spirv_load_operand(spirv_ctx, s0);
            uint32_t b = xe2_spirv_load_operand(spirv_ctx, s1);
            xe2_spirv_store_grf(spirv_ctx, dst->reg, spirv_op_fadd(&spirv_ctx->mod, spirv_ctx->fty, a, b));
            break;
        }
        case XE2_BRW_OP_MUL: {
            uint32_t a = xe2_spirv_load_operand(spirv_ctx, s0);
            uint32_t b = xe2_spirv_load_operand(spirv_ctx, s1);
            xe2_spirv_store_grf(spirv_ctx, dst->reg, spirv_op_fmul(&spirv_ctx->mod, spirv_ctx->fty, a, b));
            break;
        }
        case XE2_BRW_OP_MAD:
        case XE2_BRW_OP_MAC:
        case XE2_BRW_OP_ADD3: {
            uint32_t a = xe2_spirv_load_operand(spirv_ctx, s0);
            uint32_t b = xe2_spirv_load_operand(spirv_ctx, s1);
            uint32_t c = xe2_spirv_load_operand(spirv_ctx, s2);
            if (op == XE2_BRW_OP_ADD3) {
                uint32_t t = spirv_op_fadd(&spirv_ctx->mod, spirv_ctx->fty, a, b);
                xe2_spirv_store_grf(spirv_ctx, dst->reg, spirv_op_fadd(&spirv_ctx->mod, spirv_ctx->fty, t, c));
            } else {
                // mad: a*b + c
                uint32_t t = spirv_op_fmul(&spirv_ctx->mod, spirv_ctx->fty, a, b);
                xe2_spirv_store_grf(spirv_ctx, dst->reg, spirv_op_fadd(&spirv_ctx->mod, spirv_ctx->fty, t, c));
            }
            break;
        }
        case XE2_BRW_OP_AND:
        case XE2_BRW_OP_OR:
        case XE2_BRW_OP_XOR:
        case XE2_BRW_OP_SHR:
        case XE2_BRW_OP_SHL: {
            uint32_t u32 = spirv_type_uint32(&spirv_ctx->mod);
            uint32_t fa  = xe2_spirv_load_operand(spirv_ctx, s0);
            uint32_t fb  = xe2_spirv_load_operand(spirv_ctx, s1);
            uint32_t ia  = spirv_op_bitcast(&spirv_ctx->mod, u32, fa);
            uint32_t ib  = spirv_op_bitcast(&spirv_ctx->mod, u32, fb);
            uint32_t r;
            switch (op) {
                case XE2_BRW_OP_AND:
                    r = spirv_op_bit_and(&spirv_ctx->mod, u32, ia, ib);
                    break;
                case XE2_BRW_OP_OR:
                    r = spirv_op_bit_or(&spirv_ctx->mod, u32, ia, ib);
                    break;
                case XE2_BRW_OP_XOR:
                    r = spirv_op_bit_xor(&spirv_ctx->mod, u32, ia, ib);
                    break;
                case XE2_BRW_OP_SHR:
                    r = spirv_op_shr(&spirv_ctx->mod, u32, ia, ib);
                    break;
                default:
                    r = spirv_op_shl(&spirv_ctx->mod, u32, ia, ib);
                    break;
            }
            xe2_spirv_store_grf(spirv_ctx, dst->reg, spirv_op_bitcast(&spirv_ctx->mod, spirv_ctx->fty, r));
            break;
        }
        case XE2_BRW_OP_MATH: {
            uint32_t a = xe2_spirv_load_operand(spirv_ctx, s0);
            uint32_t v = a;
            switch (xe2_brw_math_fc(qw)) {
                case 1: // INV
                    v = spirv_op_fdiv(&spirv_ctx->mod, spirv_ctx->fty, spirv_type_const_float32(&spirv_ctx->mod, 1.0f),
                                      a);
                    break;
                case 2:
                    v = spirv_ext_inst1(&spirv_ctx->mod, spirv_ctx->fty, SPIRV_GLSL_STD450_LOG, a);
                    break;
                case 3:
                    v = spirv_ext_inst1(&spirv_ctx->mod, spirv_ctx->fty, SPIRV_GLSL_STD450_EXP, a);
                    break;
                case 4:
                    v = spirv_ext_inst1(&spirv_ctx->mod, spirv_ctx->fty, SPIRV_GLSL_STD450_SQRT, a);
                    break;
                case 5:
                    v = spirv_ext_inst1(&spirv_ctx->mod, spirv_ctx->fty, SPIRV_GLSL_STD450_INVERSE_SQRT, a);
                    break;
                case 6:
                    v = spirv_ext_inst1(&spirv_ctx->mod, spirv_ctx->fty, SPIRV_GLSL_STD450_SIN, a);
                    break;
                case 7:
                    v = spirv_ext_inst1(&spirv_ctx->mod, spirv_ctx->fty, SPIRV_GLSL_STD450_COS, a);
                    break;
                default:
                    break;
            }
            xe2_spirv_store_grf(spirv_ctx, dst->reg, v);
            break;
        }
        case XE2_BRW_OP_NOP:
        case XE2_BRW_OP_SYNC:
            break;
        default:
            // Unhandled opcode: skip without failing the whole shader.
            break;
    }
}

static void xe2_brw_print_op(const xe2_brw_operand_t* op, const xe2_qword_t* qw, char* out)
{
    if (op) {
        if (op->is_imm) {
            uint64_t imm = xe2_brw_mask(qw, 96, 127);
            sprintf(out, "imm=%lu (0x%lx)", imm, imm);
        } else {
            sprintf(out, "r%u.%u<%u:%u:%u>", op->reg, op->subreg, op->vstride, op->width, op->hstride);
        }
    }
}

static void xe2_brw_print(const xe2_qword_t* qw, uint32_t op, const xe2_brw_operand_t* dst, const xe2_brw_operand_t* s0,
                          const xe2_brw_operand_t* s1, const xe2_brw_operand_t* s2)
{
    char fmt_s0[128] = {0};
    char fmt_s1[128] = {0};
    char fmt_s2[128] = {0};

    xe2_brw_print_op(s0, qw, fmt_s0);
    xe2_brw_print_op(s1, qw, fmt_s1);
    xe2_brw_print_op(s2, qw, fmt_s2);

    rvvm_info("op:    %s r%u.%u<%u> %s %s %s", xe2_brw_op_name(op), dst->reg, dst->subreg, dst->hstride, fmt_s0, fmt_s1,
              fmt_s2);
}

static forceinline xe2_brw_operand_t xe2_brw_parse_src0(const xe2_qword_t* qw)
{
    xe2_brw_operand_t o = {0};
    if (xe2_brw_mask(qw, XE2_BRW_SRC0_IS_IMM_BIT, XE2_BRW_SRC0_IS_IMM_BIT)) {
        o.is_imm  = true;
        o.imm_u32 = xe2_brw_mask(qw, 96, 127);
        return o;
    }
    o.neg  = xe2_brw_mask(qw, XE2_BRW_SRC0_NEGATE_BIT, XE2_BRW_SRC0_NEGATE_BIT);
    o.abs  = xe2_brw_mask(qw, XE2_BRW_SRC0_ABS_BIT, XE2_BRW_SRC0_ABS_BIT);
    o.mode = xe2_brw_mask(qw, XE2_BRW_SRC0_ADDRESS_MODE_BIT, XE2_BRW_SRC0_ADDRESS_MODE_BIT);
    o.reg  = xe2_brw_mask(qw, XE2_BRW_SRC0_REG_NR_LO, XE2_BRW_SRC0_REG_NR_HI);

    uint32_t type       = xe2_brw_mask(qw, XE2_BRW_SRC0_HWTYPE_LO, XE2_BRW_SRC0_HWTYPE_HI);
    uint32_t subreg_idx = o.mode ? xe2_brw_mask(qw, XE2_BRW_SRC0_IA_SUBREG_LO, XE2_BRW_SRC0_IA_SUBREG_HI)
                                 : xe2_brw_mask(qw, XE2_BRW_SRC0_DA1_SUBREG_LO, XE2_BRW_SRC0_DA1_SUBREG_HI);

    uint32_t s0_subreg_lsb   = xe2_brw_mask(qw, XE2_BRW_SRC0_SUBREG_LSB_BIT, XE2_BRW_SRC0_SUBREG_LSB_BIT);
    uint32_t s0_subreg_bytes = (subreg_idx << 1) | s0_subreg_lsb;
    o.subreg                 = s0_subreg_bytes / xe2_brw_op_type_size(type);

    uint32_t vstride = xe2_brw_mask(qw, XE2_BRW_SRC0_VSTRIDE_LO, XE2_BRW_SRC0_VSTRIDE_HI);
    uint32_t hstride = xe2_brw_mask(qw, XE2_BRW_SRC0_HSTRIDE_LO, XE2_BRW_SRC0_HSTRIDE_HI);
    uint32_t width   = xe2_brw_mask(qw, XE2_BRW_SRC0_WIDTH_LO, XE2_BRW_SRC0_WIDTH_HI);

    o.vstride = xe2_brw_vstride_decode[vstride];
    o.hstride = xe2_brw_hstride_decode[hstride];
    o.width   = xe2_brw_width_decode[width];

    return o;
}

static forceinline xe2_brw_operand_t xe2_brw_parse_src1(const xe2_qword_t* qw)
{
    xe2_brw_operand_t o = {0};
    if (xe2_brw_mask(qw, XE2_BRW_SRC1_IS_IMM_BIT, XE2_BRW_SRC1_IS_IMM_BIT)) {
        o.is_imm  = true;
        o.imm_u32 = xe2_brw_mask(qw, 96, 127);
        return o;
    }
    o.neg  = xe2_brw_mask(qw, XE2_BRW_SRC1_NEGATE_BIT, XE2_BRW_SRC1_NEGATE_BIT);
    o.abs  = xe2_brw_mask(qw, XE2_BRW_SRC1_ABS_BIT, XE2_BRW_SRC1_ABS_BIT);
    o.mode = xe2_brw_mask(qw, XE2_BRW_SRC1_ADDRESS_MODE_BIT, XE2_BRW_SRC1_ADDRESS_MODE_BIT);
    o.reg  = xe2_brw_mask(qw, XE2_BRW_SRC1_DA_REG_NR_LO, XE2_BRW_SRC1_DA_REG_NR_HI);

    uint32_t type       = xe2_brw_mask(qw, XE2_BRW_SRC1_HWTYPE_LO, XE2_BRW_SRC1_HWTYPE_HI);
    uint32_t subreg_idx = o.mode ? xe2_brw_mask(qw, XE2_BRW_SRC1_IA_SUBREG_LO, XE2_BRW_SRC1_IA_SUBREG_HI)
                                 : xe2_brw_mask(qw, XE2_BRW_SRC1_DA1_SUBREG_LO, XE2_BRW_SRC1_DA1_SUBREG_HI);

    uint32_t s0_subreg_bytes = subreg_idx << 1;
    o.subreg                 = s0_subreg_bytes / xe2_brw_op_type_size(type);

    uint32_t vstride = xe2_brw_mask(qw, XE2_BRW_SRC1_VSTRIDE_LO, XE2_BRW_SRC1_VSTRIDE_HI);
    uint32_t hstride = xe2_brw_mask(qw, XE2_BRW_SRC1_HSTRIDE_LO, XE2_BRW_SRC1_HSTRIDE_HI);
    uint32_t width   = xe2_brw_mask(qw, XE2_BRW_SRC1_WIDTH_LO, XE2_BRW_SRC1_WIDTH_HI);

    o.vstride = xe2_brw_vstride_decode[vstride];
    o.hstride = xe2_brw_hstride_decode[hstride];
    o.width   = xe2_brw_width_decode[width];

    return o;
}

static uint32_t xe2_brw_decode_one(xe2_spirv_ctx_t* spirv_ctx, const xe2_qword_t* qw, bool* eot)
{
    bool     cmpt = (qw->lo >> XE2_BRW_CMPT_CONTROL_BIT) & 1;
    uint8_t  op   = qw->lo & 0x7F;
    uint32_t size = cmpt ? 8 : 16;

    xe2_brw_operand_t dst = {0};
    xe2_brw_operand_t s0  = {0};
    xe2_brw_operand_t s1  = {0};
    xe2_brw_operand_t s2  = {0};

    if (cmpt) {
        // Compact 1/2-src: register numbers + modifiers from index tables.
        if (xe2_brw_op_3_src(op)) {
            dst.reg     = xe2_brw_mask(qw, XE2_BRW_C3_DST_REG_NR_LO, XE2_BRW_C3_DST_REG_NR_HI);
            dst.hstride = 1 << xe2_brw_mask(qw, XE2_BRW_C_DST_HSTRIDE_LO, XE2_BRW_C_DST_HSTRIDE_HI);
            return size;
        }
        dst.reg          = xe2_brw_mask(qw, XE2_BRW_C_DST_REG_NR_LO, XE2_BRW_C_DST_REG_NR_HI);
        dst.subreg       = xe2_brw_mask(qw, XE2_BRW_DST_SUBREG_LO, XE2_BRW_DST_SUBREG_HI);
        s0.reg           = xe2_brw_mask(qw, XE2_BRW_C_SRC0_REG_NR_LO, XE2_BRW_C_SRC0_REG_NR_HI);
        s1.reg           = xe2_brw_mask(qw, XE2_BRW_C_SRC1_REG_NR_LO, XE2_BRW_C_SRC1_REG_NR_HI);
        uint32_t s0_idx  = xe2_brw_mask(qw, XE2_BRW_C_SRC0_INDEX_LO, XE2_BRW_C_SRC0_INDEX_HI);
        uint32_t s1_idx  = xe2_brw_mask(qw, XE2_BRW_C_SRC1_INDEX_LO, XE2_BRW_C_SRC1_INDEX_HI);
        uint32_t sub_idx = xe2_brw_mask(qw, XE2_BRW_C_SUBREG_INDEX_LO, XE2_BRW_C_SUBREG_INDEX_HI);
        uint16_t s0_desc = xe2_src0_index_table[s0_idx & 7];
        uint16_t s1_desc = xe2_src1_index_table[s1_idx & 15];
        s0.abs           = (s0_desc >> 0) & 1;
        s0.neg           = (s0_desc >> 1) & 1;
        if (s0_idx == 3 || s0_idx == 6) {
            s0.neg = true;
        }
        if (s0_idx == 5) {
            s0.abs = true;
        }
        s1.neg = (s1_desc >> 15) & 1;
        s1.abs = (s1_desc >> 11) & 1;

        uint16_t subreg    = xe2_subreg_table[sub_idx];
        uint16_t src0_desc = xe2_src0_index_table[s0_idx];
        uint16_t src1_desc = xe2_src1_index_table[s1_idx];

        dst.subreg = (subreg >> 0) & 0x1F;
        s0.subreg  = (subreg >> 7) & 0x1F;
        s0.abs     = (src0_desc >> 0) & 1;
        s0.neg     = (src0_desc >> 1) & 1;
        s0.hstride = xe2_brw_hstride_decode[(src0_desc >> 3) & 0x3];
        s0.width   = xe2_brw_width_decode[(src0_desc >> 5) & 0x7];
        s0.vstride = xe2_brw_vstride_decode[(src0_desc >> 8) & 0x7];

        s1.subreg  = (src1_desc >> 3) & 0x1F;
        s1.abs     = (src1_desc >> 11) & 1;
        s1.neg     = (src1_desc >> 15) & 1;
        s1.hstride = xe2_brw_hstride_decode[(src1_desc >> 3) & 0x3];
        s1.width   = xe2_brw_width_decode[(src1_desc >> 5) & 0x7];
        s1.vstride = xe2_brw_vstride_decode[(src1_desc >> 8) & 0x7];

        if (xe2_brw_op_nsrc(op) == 1) {
            xe2_brw_print(qw, op, &dst, &s0, NULL, NULL);
        } else {
            xe2_brw_print(qw, op, &dst, &s0, &s1, NULL);
        }
    } else if (xe2_brw_op_send(op)) {
        s0            = xe2_brw_parse_src0(qw);
        s1.reg        = xe2_brw_mask(qw, XE2_BRW_SEND_SRC1_REG_NR_LO, XE2_BRW_SEND_SRC1_REG_NR_HI);
        uint32_t sfid = xe2_brw_mask(qw, XE2_BRW_SEND_SFID_LO, XE2_BRW_SEND_SFID_HI);
        *eot          = xe2_brw_mask(qw, XE2_BRW_SEND_EOT_BIT, XE2_BRW_SEND_EOT_BIT);
        if (*eot) {
            spirv_ctx->saw_eot = true;
        }

        // Payload starts at s0.reg (or s1 when src0 is not a register).
        xe2_brw_emit_send(spirv_ctx, sfid, *eot, &s0, &s1);
        if (xe2_brw_op_nsrc(op) == 1) {
            xe2_brw_print(qw, op, &dst, &s0, NULL, NULL);
        } else {
            xe2_brw_print(qw, op, &dst, &s0, &s1, NULL);
        }
        return size;
    } else if (xe2_brw_op_3_src(op)) {
        dst.reg                  = xe2_brw_mask(qw, XE2_BRW_A3_DST_REG_NR_LO, XE2_BRW_A3_DST_REG_NR_HI);
        uint32_t dst_type        = xe2_brw_mask(qw, XE2_BRW_A3_DST_HWTYPE_LO, XE2_BRW_A3_DST_HWTYPE_HI);
        uint32_t dst_subreg_idx  = xe2_brw_mask(qw, XE2_BRW_A3_DST_SUBREG_LO, XE2_BRW_A3_DST_SUBREG_HI);
        uint32_t dst_hstride_bit = xe2_brw_mask(qw, XE2_BRW_A3_DST_HSTRIDE_BIT, XE2_BRW_A3_DST_HSTRIDE_BIT);
        dst.subreg               = (dst_subreg_idx << 1) / xe2_brw_op_type_size(dst_type);
        dst.hstride              = 1 << dst_hstride_bit;

        uint32_t s0_sub_idx = xe2_brw_mask(qw, XE2_BRW_A3_SRC0_SUBREG_LO, XE2_BRW_A3_SRC0_SUBREG_HI);
        uint32_t s0_type    = xe2_brw_mask(qw, XE2_BRW_A3_SRC0_HWTYPE_LO, XE2_BRW_A3_SRC0_HWTYPE_HI);
        s0.subreg           = (s0_sub_idx << 1) / xe2_brw_op_type_size(s0_type);
        s0.reg              = xe2_brw_mask(qw, XE2_BRW_A3_SRC0_REG_NR_LO, XE2_BRW_A3_SRC0_REG_NR_HI);
        s0.neg              = xe2_brw_mask(qw, XE2_BRW_A3_SRC0_NEGATE_BIT, XE2_BRW_A3_SRC0_NEGATE_BIT);
        s0.abs              = xe2_brw_mask(qw, XE2_BRW_A3_SRC0_ABS_BIT, XE2_BRW_A3_SRC0_ABS_BIT);
        s0.hstride          = xe2_brw_mask(qw, XE2_BRW_A3_SRC0_HSTRIDE_LO, XE2_BRW_A3_SRC0_HSTRIDE_HI);
        s0.vstride          = xe2_brw_3_src_vstride(s0.hstride);
        uint32_t s1_sub_idx = xe2_brw_mask(qw, XE2_BRW_A3_SRC1_SUBREG_LO, XE2_BRW_A3_SRC1_SUBREG_HI);
        uint32_t s1_type    = xe2_brw_mask(qw, XE2_BRW_A3_SRC1_HWTYPE_LO, XE2_BRW_A3_SRC1_HWTYPE_HI);
        s1.subreg           = (s1_sub_idx << 1) / xe2_brw_op_type_size(s1_type);
        s1.reg              = xe2_brw_mask(qw, XE2_BRW_A3_SRC1_REG_NR_LO, XE2_BRW_A3_SRC1_REG_NR_HI);
        s1.neg              = xe2_brw_mask(qw, XE2_BRW_A3_SRC1_NEGATE_BIT, XE2_BRW_A3_SRC1_NEGATE_BIT);
        s1.abs              = xe2_brw_mask(qw, XE2_BRW_A3_SRC1_ABS_BIT, XE2_BRW_A3_SRC1_ABS_BIT);
        s1.hstride          = xe2_brw_mask(qw, XE2_BRW_A3_SRC1_HSTRIDE_LO, XE2_BRW_A3_SRC1_HSTRIDE_HI);
        s1.vstride          = xe2_brw_3_src_vstride(s1.hstride);
        uint32_t s2_sub_idx = xe2_brw_mask(qw, XE2_BRW_A3_SRC2_SUBREG_LO, XE2_BRW_A3_SRC2_SUBREG_HI);
        uint32_t s2_type    = xe2_brw_mask(qw, XE2_BRW_A3_SRC2_HWTYPE_LO, XE2_BRW_A3_SRC2_HWTYPE_HI);
        s2.subreg           = (s2_sub_idx << 1) / xe2_brw_op_type_size(s2_type);
        s2.reg              = xe2_brw_mask(qw, XE2_BRW_A3_SRC2_REG_NR_LO, XE2_BRW_A3_SRC2_REG_NR_HI);
        s2.neg              = xe2_brw_mask(qw, XE2_BRW_A3_SRC2_NEGATE_BIT, XE2_BRW_A3_SRC2_NEGATE_BIT);
        s2.abs              = xe2_brw_mask(qw, XE2_BRW_A3_SRC2_ABS_BIT, XE2_BRW_A3_SRC2_ABS_BIT);
        s2.hstride          = xe2_brw_mask(qw, XE2_BRW_A3_SRC2_HSTRIDE_LO, XE2_BRW_A3_SRC2_HSTRIDE_HI);
        s2.vstride          = xe2_brw_3_src_vstride(s2.hstride);
        xe2_brw_print(qw, op, &dst, &s0, &s1, &s2);
    } else {
        dst.reg = xe2_brw_parse_dst(qw);
        s0      = xe2_brw_parse_src0(qw);
        s1      = xe2_brw_parse_src1(qw);
        if (xe2_brw_op_nsrc(op) == 1) {
            xe2_brw_print(qw, op, &dst, &s0, NULL, NULL);
        } else {
            xe2_brw_print(qw, op, &dst, &s0, &s1, NULL);
        }
    }

    xe2_brw_emit_spirv(spirv_ctx, qw, op, &dst, &s0, &s1, &s2);
    return size;
}

static void xe2_write_spirv(const uint32_t* words, uint32_t n)
{
    int spirv_fd = open("compiled.spirv", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (spirv_fd < 0) {
        perror("compiled.spirv");
        return;
    }
    if (write(spirv_fd, words, n * sizeof(uint32_t)) < 0) {
        perror("write");
    }
    close(spirv_fd);
    rvvm_info(" ");
    rvvm_info(" ");
    rvvm_info("-- Written SPIR-V shader");
    rvvm_info(" ");
    if (system("spirv-dis compiled.spirv") != 0) {
        rvvm_warn("spirv-dis failed");
    }
    // Catches a module the driver would reject before it reaches one.
    if (system("spirv-val --target-env vulkan1.1 compiled.spirv") != 0) {
        rvvm_warn("spirv-val rejected the compiled shader");
    }
}

// Cross-compile the BRW kernel at the given address into a SPIR-V module
// for the stage. The module scaffolding (register file, pushed constant
// block, stage outputs, entry point) is built by gpu-xe2-shader.h; this
// walks the kernel and feeds its instructions through the translator.
static void xe2_brw_decode(xe2_dev_t* xe2, xe2_shader_kind_t kind, xe2_dma_addr_t dma, uint32_t** spirv,
                           uint32_t* spirv_nwords)
{
    if (dma.addr == 0U) {
        return;
    }

    xe2_spirv_begin(&xe2->spirv_ctx, kind);

    static const uint32_t limit = 4096;
    uint32_t              len   = 0U;
    uint32_t              zeros = 0U;
    size_t                off   = 0ULL;

    for (uint32_t i = 0; i < limit; ++i) {
        xe2_qword_t qw = {0};
        xe2_dma_read_many(xe2, xe2_dma_offset(dma, off), &qw, sizeof(qw) / sizeof(uint32_t));
        if (qw.hi == 0U) {
            if (++zeros >= 4 && len > 0) {
                break;
            }
        } else {
            zeros = 0;
            len   = i + 1;
        }
        bool eot  = 0;
        off      += xe2_brw_decode_one(&xe2->spirv_ctx, &qw, &eot);
        if (eot) {
            break;
        }
    }

    if (!xe2->spirv_ctx.wrote_output) {
        rvvm_warn("(kind: %u) No output message recognised in kernel, shader writes a default", kind);
    }

    if (xe2_spirv_finish(&xe2->spirv_ctx, spirv, spirv_nwords) != 0) {
        rvvm_warn("(kind: %u) Failed to serialize SPIR-V module", kind);
        return;
    }
    rvvm_info("Vulkan shader: %p, %u dwords", (void*)*spirv, *spirv_nwords);
    xe2_write_spirv(*spirv, *spirv_nwords);
}

// -----------------------------------------------------------
// Push constants (3DSTATE_CONSTANT_XS)
// -----------------------------------------------------------



// Upper bound on the gathered payload we mirror. Also the size of the
// uniform block the cross-compiled shader declares and of the buffer
// the Vulkan backend uploads, so all three agree by construction.
#define XE2_CONST_MAX_VEC4   64
#define XE2_CONST_MAX_DWORDS (XE2_CONST_MAX_VEC4 * 4)
#define XE2_CONST_MAX_BYTES  (XE2_CONST_MAX_DWORDS * 4)

// The uniform block a cross-compiled shader declares for its pushed
// constants and the buffer the backend binds to it are the same object
// seen from two sides, so their sizes have to agree.
BUILD_ASSERT(XE2_CONST_MAX_BYTES == GPU_VULKAN_CONST_BYTES);
BUILD_ASSERT(XE2_SHADER_CONST_SET == GPU_VULKAN_CONST_SET);

// So do the binding numbers: the shader is compiled with its Xe2 stage
// index as the binding, while the backend lays the descriptor set out by
// Vulkan stage index. See xe2_draw_stages[] for the correspondence.
BUILD_ASSERT((int)XE2_SHADER_VS == (int)GPU_VULKAN_STAGE_VERTEX);
BUILD_ASSERT((int)XE2_SHADER_HS == (int)GPU_VULKAN_STAGE_TESS_CTRL);
BUILD_ASSERT((int)XE2_SHADER_DS == (int)GPU_VULKAN_STAGE_TESS_EVAL);
BUILD_ASSERT((int)XE2_SHADER_GS == (int)GPU_VULKAN_STAGE_GEOMETRY);
BUILD_ASSERT((int)XE2_SHADER_PS == (int)GPU_VULKAN_STAGE_FRAGMENT);

// Decode the four (read length, address) pairs of a 3DSTATE_CONSTANT_XS
// command into the stage's constant state. cmd points at the command
// header, so indices below are command dwords:
//
//   [0]      header
//   [1]      Read Length[0] (15:0), Read Length[1] (31:16)
//   [2]      Read Length[2] (15:0), Read Length[3] (31:16)
//   [3..4]   Buffer[0], 64-bit, 32-byte aligned (bits 63:5)
//   [5..6]   Buffer[1]
//   [7..8]   Buffer[2]
//   [9..10]  Buffer[3]
//
// Leaves the gathered payload alone - reading it needs DMA, which the
// caller does in xe2_push_const_gather().
#define XE2_CONST_CMD_DWORDS 11

static inline void xe2_const_body_decode(const uint32_t* cmd, xe2_push_const_t* consts)
{
    for (uint32_t i = 0; i < XE2_CONST_BUFFERS; ++i) {
        uint32_t len_dw = cmd[1 + (i >> 1)];
        uint32_t lo     = cmd[3 + i * 2];
        uint32_t hi     = cmd[4 + i * 2];

        consts->buffer[i].read_length = (i & 1) ? (len_dw >> 16) : (len_dw & 0xFFFF);
        // Bits 63:5 - the buffer is 32-byte aligned, low bits are MOCS.
        consts->buffer[i].va = xe2_concat_lohi(lo, hi);
    }
}

// Has anything the renderer cares about changed since the last draw?
static inline bool xe2_3dstate_dirty(const xe2_3dstate_t* d3d, const xe2_draw_params_t* draw)
{
    if (!d3d->last_draw_valid || d3d->ff_dirty) {
        return true;
    }
    for (uint32_t i = 0; i < XE2_SHADER_STAGE_COUNT; ++i) {
        if (d3d->shader[i].dirty || d3d->consts[i].dirty) {
            return true;
        }
    }
    return memcmp(draw, &d3d->last_draw, sizeof(*draw)) != 0;
}

// -----------------------------------------------------------
// Logical Ring Context (LRC), GPU commands processing
// -----------------------------------------------------------



// Read a dword from the registered context's LRC register state.
static inline uint32_t xe2_lrc_reg_read(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, uint32_t idx)
{
    return xe2_dma_read_32(xe2, ctx->pphwsp, XE2_LRC_REGS_OFFSET + idx * 4);
}

// Write a dword to the registered context's LRC register state.
static inline void xe2_lrc_reg_write(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, uint32_t idx, uint32_t msg)
{
    xe2_dma_write_32(xe2, ctx->pphwsp, XE2_LRC_REGS_OFFSET + idx * 4, msg);
}

// Perform a ring post-sync store: write a value to a GGTT address.
static void xe2_ring_store(xe2_dev_t* xe2, uint32_t ggtt_addr, uint32_t value)
{
    if (unlikely(ggtt_addr == 0)) {
        return;
    }
    xe2_dma_addr_t dst = xe2_ggtt_translate(xe2, ggtt_addr);
    if (unlikely(dst.addr == 0)) {
        return;
    }
    xe2_dma_write_32(xe2, dst, 0, value);
}

// The supplied ring DMA address is normalized such that the first dword is the
// currently processed instruction header (opcode).
//
// Forward declaration due to nested handling inside BATCH_BUFFER_START.
static uint32_t xe2_ring_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_dma_addr_t ring, rvvm_addr_t pdp4, uint32_t op,
                             bool* user_int);

static inline uint32_t xe2_process_batch_buffer(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, rvvm_addr_t pdp4, uint32_t op,
                                                rvvm_addr_t bo, bool* user_int)
{
    // From the command processor's point of view, we treat GGTT/PPGTT-allocated
    // rings in exactly the same way.
    xe2_dma_addr_t ring = {0};
    if (op & XE2_MI_OP_BATCH_BUFFER_START_PPGTT) {
        ring = xe2_ppgtt_translate(xe2, pdp4, bo);
    } else {
        ring = xe2_ggtt_translate(xe2, bo);
    }

    if (unlikely(ring.addr == 0ULL)) {
        rvvm_warn("Failed to translate batch buffer address! bo: 0x%" PRIx64, bo);
        return 3;
    }

    uint64_t i = 0ULL;
    // Process ring commands until XE2_MI_OP_BATCH_BUFFER_END will be found.
    // Ring iterator will be advanced by instruction length reported by a
    // specific command handler (MI/GFXPIPE).
    while (true) {
        uint32_t ring_op = xe2_dma_read_32(xe2, ring, i * 4);
        if (XE2_INSTR_TYPE(ring_op) == XE2_INSTR_TYPE_MI && XE2_MI_OPCODE(ring_op) == XE2_MI_OP_BATCH_BUFFER_END) {
            break;
        }

        // Advance by reported from command handler length.
        i += xe2_ring_cmd(xe2, ctx, xe2_dma_offset(ring, i * 4), pdp4, ring_op, user_int);
    }

    return 3;
}

// The supplied ring DMA address is normalized such that the first dword is the
// currently processed instruction header (opcode).
static inline uint32_t xe2_mi_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_dma_addr_t ring, rvvm_addr_t pdp4,
                                  uint32_t op, bool* user_int)
{
    // rvvm_info("MI opcode: 0x%x", XE2_MI_OPCODE(op));

    switch (XE2_MI_OPCODE(op)) {
        case XE2_MI_OP_NOOP:
        case XE2_MI_OP_ARB_CHECK:
        case XE2_MI_OP_ARB_ON_OFF:
        case XE2_MI_OP_BATCH_BUFFER_END:
            return 1;
        case XE2_MI_OP_USER_INTERRUPT:
            *user_int = true;
            return 1;
        case XE2_MI_OP_STORE_DATA_IMM:
            if (op & XE2_MI_SDI_GGTT) {
                uint32_t a = xe2_dma_read_32(xe2, ring, 1 * 4);
                uint32_t v = xe2_dma_read_32(xe2, ring, 3 * 4);
                xe2_ring_store(xe2, a, v);
            }
            return (op & 0x3FF) + 2;
        case XE2_MI_OP_FLUSH_DW:
            if (op & XE2_MI_FLUSH_DW_OP_STOREDW) {
                uint32_t a = xe2_dma_read_32(xe2, ring, 1 * 4);
                uint32_t v = xe2_dma_read_32(xe2, ring, 3 * 4);
                if (a & XE2_MI_FLUSH_DW_USE_GTT) {
                    xe2_ring_store(xe2, a & ~0x7U, v);
                }
            }
            return (op & 0x3F) + 2;
        case XE2_MI_OP_BATCH_BUFFER_START: {
            uint32_t    lo = xe2_dma_read_32(xe2, ring, 1 * 4);
            uint32_t    hi = xe2_dma_read_32(xe2, ring, 2 * 4);
            rvvm_addr_t bo = xe2_concat_lohi(lo, hi);
            return xe2_process_batch_buffer(xe2, ctx, pdp4, op, bo, user_int);
        }
        default:
            return (op & 0xFF) + 2;
    }
}

// This follows common [63:12] convention for multi-dword address encoding:
//
// lo: [xxxxxxxxxxxxxxxxxxxx000000000000]
//      |     payload      |
// hi: [xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx]
//      |     payload << 32            |
static inline rvvm_addr_t xe2_addr_63_12_mask(uint32_t lo, uint32_t hi)
{
    return (rvvm_addr_t)hi << 32 | (lo & 0xFFFFF000);
}

// This follows common [63:6] convention for multi-dword address encoding:
//
// lo: [xxxxxxxxxxxxxxxxxxxxxxxxxx000000]
//      |     payload            |
// hi: [xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx]
//      |     payload << 32            |
static inline rvvm_addr_t xe2_addr_63_6_mask(uint32_t lo, uint32_t hi)
{
    return (rvvm_addr_t)hi << 32 | (lo & 0xFFFFFFC0);
}

// Function was forged only for debugging purposes.
static void xe2_print_decompiled_shader(xe2_dev_t* xe2, xe2_dma_addr_t dma)
{
    if (dma.addr == 0U) {
        return;
    }

    static const uint32_t limit      = 4096;
    uint32_t              code[4096] = {0};
    uint32_t              len        = 0U;
    uint32_t              zeros      = 0U;

    for (uint32_t i = 0; i < limit; ++i) {
        uint32_t op = xe2_dma_read_32(xe2, dma, i * 4);
        code[i]     = op;
        if (op == 0U) {
            if (++zeros >= 4 && len > 0) {
                break;
            }
        } else {
            zeros = 0;
            len   = i + 1;
        }
    }

    int fd = open("decompiled.xe2", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        rvvm_warn("cannot open decompiled.xe2: %s", strerror(errno));
        return;
    }

    uint32_t dump_len = len > limit ? limit : len;

    if (write(fd, code, dump_len * sizeof(uint32_t)) == (ssize_t)-1) {
        rvvm_warn("write() failed");
    }
    rvvm_info("dumped %u dwords (%u bytes) into decompiled.xe2", dump_len, dump_len * 4);

    if (system("xxd decompiled.xe2") != 0) {
        rvvm_warn("xxd failed");
    }
    if (system("iga64 -d -p 2 decompiled.xe2 2>&1") != 0) {
        rvvm_warn("iga64 failed");
    }
}

static void xe2_decode_shader(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_shader_kind_t kind, rvvm_addr_t pdp4,
                              rvvm_addr_t addr_kernel, uint64_t addr_instr)
{
    xe2_shader_stage_t* stage = &ctx->d3d.shader[kind];
    rvvm_addr_t         va    = addr_kernel + addr_instr;

    // The driver re-emits 3DSTATE_XS on every batch even when nothing
    // changed; recompiling the same kernel each time would be wasted
    // work. This takes the kernel address as the kernel's identity,
    // which holds as long as the driver allocates a new buffer for a new
    // kernel rather than overwriting one in place.
    if (stage->spirv && stage->kernel_va == va) {
        stage->enabled = true;
        return;
    }

    rvvm_info("(kind: %u) Kernel start address: 0x%lx", kind, addr_kernel);
    xe2_dma_addr_t kernel_dma = xe2_ppgtt_translate(xe2, pdp4, va);
    xe2_print_decompiled_shader(xe2, kernel_dma);

    uint32_t* spirv        = NULL;
    uint32_t  spirv_nwords = 0;
    xe2_brw_decode(xe2, kind, kernel_dma, &spirv, &spirv_nwords);
    if (!spirv || !spirv_nwords) {
        return;
    }

    free(stage->spirv);
    stage->spirv         = spirv;
    stage->spirv_nwords  = spirv_nwords;
    stage->kernel_va     = va;
    stage->push_grf_base = xe2_push_const_grf_base(kind);
    stage->enabled       = true;
    stage->dirty         = true;
}

static xe2_shader_kind_t xe2_constant_cmd_to_stage(uint32_t cmd)
{
    switch (cmd) {
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_VS:
            return XE2_SHADER_VS;
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_HS:
            return XE2_SHADER_HS;
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_DS:
            return XE2_SHADER_DS;
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_GS:
            return XE2_SHADER_GS;
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_PS:
            return XE2_SHADER_PS;
        default:
            rvvm_fatal("Unknown 3DSTATE shader kind: %u", cmd);
            return XE2_SHADER_VS;
    }
}

static xe2_shader_kind_t xe2_binding_table_cmd_to_stage(uint32_t cmd)
{
    switch (cmd) {
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_VS:
            return XE2_SHADER_VS;
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_HS:
            return XE2_SHADER_HS;
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_DS:
            return XE2_SHADER_DS;
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_GS:
            return XE2_SHADER_GS;
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_PS:
            return XE2_SHADER_PS;
        default:
            rvvm_fatal("Unknown 3DSTATE shader kind: %u", cmd);
            return XE2_SHADER_VS;
    }
}

// Reads a constant buffer's contents out of guest memory into the
// stage's gathered payload, at the offset the hardware would have
// placed it. Returns the number of bytes taken.
static uint32_t xe2_const_buffer_fetch(xe2_dev_t* xe2, rvvm_addr_t pdp4, const xe2_const_buffer_t* buf, uint8_t* dst,
                                       uint32_t space)
{
    uint32_t nbytes = buf->read_length * XE2_CONST_CHUNK_BYTES;
    if (!buf->va || !nbytes) {
        return 0;
    }
    if (nbytes > space) {
        rvvm_warn("Constant buffer truncated: %u of %u bytes fit", space, nbytes);
        nbytes = space;
    }

    xe2_dma_addr_t dma = xe2_ppgtt_translate(xe2, pdp4, buf->va);
    if (!dma.addr) {
        rvvm_warn("Failed to translate constant buffer address 0x%" PRIx64, buf->va);
        return 0;
    }
    xe2_dma_read_bytes(xe2, dma, dst, nbytes);
    return nbytes;
}

// 3DSTATE_CONSTANT_XS handler: decode the four buffers the command
// programs, then gather their contents into the stage's payload - the
// same byte image the hardware would push into the thread's GRFs, and
// the one the cross-compiled shader reads through its uniform block.
static void xe2_3dstate_constant(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_shader_kind_t kind, rvvm_addr_t pdp4,
                                 const uint32_t* cmd)
{
    xe2_push_const_t* consts = &ctx->d3d.consts[kind];

    xe2_const_body_decode(cmd, consts);

    uint32_t off      = 0;
    uint32_t gathered = 0;
    for (uint32_t i = 0; i < XE2_CONST_BUFFERS; ++i) {
        uint32_t taken
            = xe2_const_buffer_fetch(xe2, pdp4, &consts->buffer[i], consts->payload + off, XE2_CONST_MAX_BYTES - off);
        off      += taken;
        gathered += taken ? 1 : 0;
    }
    // Stale bytes from a previous, larger payload would otherwise leak
    // into the uniform block the shader reads.
    memset(consts->payload + off, 0, XE2_CONST_MAX_BYTES - off);

    consts->nbytes = off;
    consts->dirty  = true;

    rvvm_info("(kind: %u) Pushed constants: %u bytes from %u buffer(s)", kind, off, gathered);
    for (uint32_t i = 0; i < off / 4 && i < 16; ++i) {
        uint32_t dw = read_uint32_le(consts->payload + i * 4);
        float    f;
        memcpy(&f, &dw, sizeof(f));
        rvvm_info("  c[%u][%u] = 0x%08x (%f)", i / 4, i % 4, dw, (double)f);
    }
}

// The stages the graphics pipeline can carry, in pipeline order. The
// compute stage has no place in a draw, so it is not in this table.
static const struct {
    xe2_shader_kind_t  xe2;
    gpu_vulkan_stage_t vk;
} xe2_draw_stages[] = {
    {XE2_SHADER_VS,    GPU_VULKAN_STAGE_VERTEX},
    {XE2_SHADER_HS, GPU_VULKAN_STAGE_TESS_CTRL},
    {XE2_SHADER_DS, GPU_VULKAN_STAGE_TESS_EVAL},
    {XE2_SHADER_GS,  GPU_VULKAN_STAGE_GEOMETRY},
    {XE2_SHADER_PS,  GPU_VULKAN_STAGE_FRAGMENT},
};

// 3D_PRIM_TOPOLOGY_TYPE -> VkPrimitiveTopology. Only the topologies with
// a direct Vulkan counterpart are translated; adjacency and patch lists
// fall back to a triangle list.
static uint32_t xe2_topology_to_vulkan(uint32_t topology)
{
    switch (topology) {
        case 0x01:
            return GPU_VULKAN_TOPOLOGY_POINT_LIST;
        case 0x02:
            return GPU_VULKAN_TOPOLOGY_LINE_LIST;
        case 0x03:
            return GPU_VULKAN_TOPOLOGY_LINE_STRIP;
        case 0x05:
            return GPU_VULKAN_TOPOLOGY_TRIANGLE_STRIP;
        case 0x06:
            return GPU_VULKAN_TOPOLOGY_TRIANGLE_FAN;
        case 0x04:
        default:
            return GPU_VULKAN_TOPOLOGY_TRIANGLE_LIST;
    }
}

static inline void xe2_surface_print(const char* name, xe2_surface_state_t* surface)
{
    if (surface->valid) {
        rvvm_info(" | (%s) ISL format:        %u", name, surface->isl_format);
        rvvm_info(" | (%s) Tile mode:         %u", name, surface->tile_mode);
        rvvm_info(" | (%s) Width/Height:      %u/%u", name, surface->width, surface->height);
        rvvm_info(" | (%s) Pitch:             %u", name, surface->pitch);
    } else {
        rvvm_info(" | (%s) ... Empty", name);
    }
}

static inline void xe2_3dprimitive_print(xe2_submit_ctx_t* ctx)
{
    rvvm_info("3DPRIMITIVE state dump");
    rvvm_info(" Surface states:");

    if (ctx->d3d.shader[XE2_SHADER_PS].enabled) {
        xe2_surface_print("PS", &ctx->d3d.surface[XE2_SHADER_PS][0]);
    }
    if (ctx->d3d.shader[XE2_SHADER_VS].enabled) {
        xe2_surface_print("VS", &ctx->d3d.surface[XE2_SHADER_VS][0]);
    }
    if (ctx->d3d.shader[XE2_SHADER_HS].enabled) {
        xe2_surface_print("HS", &ctx->d3d.surface[XE2_SHADER_HS][0]);
    }
    if (ctx->d3d.shader[XE2_SHADER_DS].enabled) {
        xe2_surface_print("DS", &ctx->d3d.surface[XE2_SHADER_DS][0]);
    }
    if (ctx->d3d.shader[XE2_SHADER_GS].enabled) {
        xe2_surface_print("GS", &ctx->d3d.surface[XE2_SHADER_GS][0]);
    }

    rvvm_info(" (PS) Shader enabled? %d", ctx->d3d.shader[XE2_SHADER_PS].enabled);
    rvvm_info(" (VS) Shader enabled? %d", ctx->d3d.shader[XE2_SHADER_VS].enabled);
    rvvm_info(" (HS) Shader enabled? %d", ctx->d3d.shader[XE2_SHADER_HS].enabled);
    rvvm_info(" (DS) Shader enabled? %d", ctx->d3d.shader[XE2_SHADER_DS].enabled);
    rvvm_info(" (GS) Shader enabled? %d", ctx->d3d.shader[XE2_SHADER_GS].enabled);
}

// Turn the accumulated 3D state into a draw for the Vulkan backend.
//
// Vertex attributes are not wired yet: a cross-compiled kernel reads its
// varyings out of the URB payload registers, which the register model
// does not reproduce, so vertex buffers and vertex element descriptions
// have no consumer. Constants do have one - that is the path this
// submits, and what a kernel computes from them is what ends up on
// screen.
static void xe2_3dprimitive(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, uint32_t* cmd)
{
    xe2_3dprimitive_print(ctx);

    bool     indexed        = (cmd[1] >> 8) & 1; // VertexAccessType
    uint32_t vertex_count   = cmd[2];
    uint32_t start_vertex   = cmd[3];
    uint32_t instance_count = cmd[4];
    uint32_t start_instance = cmd[5];
    int32_t  base_vertex    = (int32_t)cmd[6];

    rvvm_info("%s: vertex_count: %u, start_vertex: %u, instances: %u, start_instance: %u, base_vertex: %u",
              __FUNCTION__, vertex_count, start_vertex, instance_count, start_instance, base_vertex);
    if (indexed) {
        rvvm_info("%s: indexed draws are not translated yet", __FUNCTION__);
    }

    xe2_3dstate_t* d3d = &ctx->d3d;

    xe2_draw_params_t params = {
        .topology       = xe2_topology_to_vulkan(d3d->vertex_input.topology),
        .vertex_count   = vertex_count,
        .instance_count = instance_count ? instance_count : 1,
        .first_vertex   = start_vertex,
        .first_instance = start_instance,
    };

    // A batch that re-emits the same state and repeats the same draw
    // gets the same picture; handing it over again would only re-copy
    // every shader module for nothing.
    if (!xe2_3dstate_dirty(d3d, &params)) {
        rvvm_warn("%s: !xe2_3dstate_dirty()", __FUNCTION__);
        return;
    }

    gpu_vulkan_draw_t draw = {
        .topology       = params.topology,
        .vertex_count   = params.vertex_count,
        .instance_count = params.instance_count,
        .first_vertex   = params.first_vertex,
        .first_instance = params.first_instance,
    };

    // BUG: Not each stage, only submitted ones.
    for (size_t i = 0; i < STATIC_ARRAY_SIZE(xe2_draw_stages); ++i) {
        xe2_shader_kind_t         kind   = xe2_draw_stages[i].xe2;
        const xe2_shader_stage_t* stage  = &d3d->shader[kind];
        const xe2_push_const_t*   consts = &d3d->consts[kind];

        rvvm_info("%s: Write stage (vertex? %d, kind: %s, spirv: %p, words: %u)", __FUNCTION__,
                  xe2_draw_stages[i].vk == GPU_VULKAN_STAGE_VERTEX, gpu_vulkan_stage_to_string(xe2_draw_stages[i].vk),
                  stage->spirv, stage->spirv_nwords);
        if (/* !stage->enabled || */ !stage->spirv) {
            // This could happen when 3DPRIMITIVE was already issued without
            // submitting kernel addresses (3DSTATE_PS/VS/HS/...). I have no
            // idea why that happens, but that happens.
            continue;
        }

        draw.stage[xe2_draw_stages[i].vk] = (gpu_vulkan_stage_desc_t) {
            .spirv        = stage->spirv,
            .spirv_nwords = stage->spirv_nwords,
            .constants    = consts->payload,
            .const_bytes  = consts->nbytes,
        };

        rvvm_info("%s: Count %u ", __FUNCTION__, d3d->binding_table_entry_count[kind]);
        for (uint32_t t = 0; t < d3d->binding_table_entry_count[kind]; ++t) {
            const xe2_surface_state_t* surf = &d3d->surface[kind][t];
            rvvm_info(
                "%s: stage %s binds surface %u ([%u][%u]): %ux%u isl_format=%u tile_mode=%u pitch=%u base=0x%" PRIx64,
                __FUNCTION__, gpu_vulkan_stage_to_string(xe2_draw_stages[i].vk), t, kind, t, surf->width, surf->height,
                surf->isl_format, surf->tile_mode, surf->pitch, surf->base.addr);
        }
        // TODO(vulkan-textures): gpu_vulkan_stage_desc_t has no image
        // field yet. It needs one shaped like {isl_format, width, height,
        // pitch, tile_mode, dereferenceable bytes} per bound surface -
        // the same "hand over a plain pointer, let the backend own
        // VkImage/VkSampler/descriptor-set creation" pattern .spirv and
        // .constants already use above. TILE4 surfaces (this device's
        // only non-linear mode) need de-tiling into a linear staging
        // buffer before vkCmdCopyBufferToImage; LINEAR ones can copy
        // through as-is via xe2_dma_read_bytes(). Sampler state
        // (3DSTATE_SAMPLER_STATE_POINTERS_PS -> SAMPLER_STATE, opcodes
        // already #defined above, not yet decoded) is the matching piece
        // for filter/wrap modes and belongs in the same struct.
    }

    bool submitted = xe2->vulkan_ctx && gpu_vulkan_submit_draw(xe2->vulkan_ctx, &draw);
    if (!submitted) {
        rvvm_warn("%s: draw carries no usable shaders, skipped", __FUNCTION__);
    }

    for (uint32_t s = 0; s < XE2_SHADER_STAGE_COUNT; ++s) {
        d3d->shader[s].dirty = false;
        d3d->consts[s].dirty = false;
    }
    d3d->ff_dirty        = false;
    d3d->last_draw       = params;
    d3d->last_draw_valid = true;

    // Only an actually-accepted draw counts as "guest is driving the 3D
    // pipeline" - a rejected/shaderless draw must not flip scanout away
    // from the plane's raw buffer.
    if (submitted) {
        xe2->draw_submitted = true;
        xe2->last_draw_tick = xe2->scanout_tick;
    }
}

static inline void xe2_gfxpipe_pipe_control_cmd(xe2_dev_t* xe2, xe2_dma_addr_t ring)
{
    uint32_t flags = xe2_dma_read_32(xe2, ring, 1 * 4);
    if ((flags & XE2_GFXPIPE_CMD_PIPE_CONTROL_QW_WRITE) && (flags & XE2_GFXPIPE_CMD_PIPE_CONTROL_GLOBAL_GTT)) {
        uint32_t a = xe2_dma_read_32(xe2, ring, 2 * 4);
        uint32_t v = xe2_dma_read_32(xe2, ring, 4 * 4);
        xe2_ring_store(xe2, a, v);
    }
}

static inline void xe2_3dstate_ps_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, rvvm_addr_t pdp4, xe2_dma_addr_t ring)
{
    uint32_t cmd[12] = {0};
    xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
    rvvm_addr_t addr_kernel[] = {
        xe2_addr_63_6_mask(cmd[1], cmd[2]),
        xe2_addr_63_6_mask(cmd[8], cmd[9]),
    };
    bool addr_kernel_enable[] = {cmd[0] & 1, cmd[8] & 1};
    bool any                  = false;
    for (size_t i = 0; i < STATIC_ARRAY_SIZE(addr_kernel); ++i) {
        if (addr_kernel_enable[i]) {
            rvvm_info("(PS) kernel %zu: lx%0lx", i, addr_kernel[i]);
            xe2_decode_shader(xe2, ctx, XE2_SHADER_PS, pdp4, addr_kernel[i], ctx->addr_instr);
            any = true;
        }
    }
    if (!any) {
        ctx->d3d.shader[XE2_SHADER_PS].enabled = false;
    }
    ctx->d3d.binding_table_entry_count[XE2_SHADER_PS] = xe2_reg_field_get(xe2_reg_genmask(25, 18), cmd[3]);
    rvvm_info("(PS) Binding table entry count: %u", ctx->d3d.binding_table_entry_count[XE2_SHADER_PS]);
}

static inline void xe2_3dstate_vs_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, rvvm_addr_t pdp4, xe2_dma_addr_t ring)
{
    uint32_t cmd[9] = {0};
    xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
    bool enable = cmd[7] & 1;
    if (enable) {
        rvvm_addr_t addr_kernel = xe2_addr_63_6_mask(cmd[1], cmd[2]);
        rvvm_info("(VS) kernel: lx%0lx", addr_kernel);
        xe2_decode_shader(xe2, ctx, XE2_SHADER_VS, pdp4, addr_kernel, ctx->addr_instr);
    } else {
        ctx->d3d.shader[XE2_SHADER_VS].enabled = false;
    }
    ctx->d3d.binding_table_entry_count[XE2_SHADER_VS] = xe2_reg_field_get(xe2_reg_genmask(25, 18), cmd[3]);
    rvvm_info("(VS) Binding table entry count: %u", ctx->d3d.binding_table_entry_count[XE2_SHADER_VS]);
}

static inline void xe2_3dstate_gs_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, rvvm_addr_t pdp4, xe2_dma_addr_t ring)
{
    uint32_t cmd[10] = {0};
    xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
    bool enable = cmd[7] & 1;
    if (enable) {
        rvvm_addr_t addr_kernel = xe2_addr_63_6_mask(cmd[1], cmd[2]);
        rvvm_info("(GS) kernel: lx%0lx", addr_kernel);
        xe2_decode_shader(xe2, ctx, XE2_SHADER_GS, pdp4, addr_kernel, ctx->addr_instr);
    } else {
        ctx->d3d.shader[XE2_SHADER_GS].enabled = false;
    }
    ctx->d3d.binding_table_entry_count[XE2_SHADER_GS] = xe2_reg_field_get(xe2_reg_genmask(25, 18), cmd[3]);
    rvvm_info("(GS) Binding table entry count: %u", ctx->d3d.binding_table_entry_count[XE2_SHADER_GS]);
}

static inline void xe2_3dstate_hs_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, rvvm_addr_t pdp4, xe2_dma_addr_t ring)
{
    uint32_t cmd[9] = {0};
    xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
    bool enable = (cmd[2] >> 31) & 1;
    if (enable) {
        rvvm_addr_t addr_kernel = xe2_addr_63_6_mask(cmd[3], cmd[4]);
        rvvm_info("(HS) kernel: lx%0lx", addr_kernel);
        xe2_decode_shader(xe2, ctx, XE2_SHADER_HS, pdp4, addr_kernel, ctx->addr_instr);
    } else {
        ctx->d3d.shader[XE2_SHADER_HS].enabled = false;
    }
    ctx->d3d.binding_table_entry_count[XE2_SHADER_HS] = xe2_reg_field_get(xe2_reg_genmask(25, 18), cmd[1]);
    rvvm_info("(HS) Binding table entry count: %u", ctx->d3d.binding_table_entry_count[XE2_SHADER_HS]);
}

static inline void xe2_3dstate_vertex_buffers_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_dma_addr_t ring,
                                                  rvvm_addr_t pdp4, uint32_t op)
{
    xe2_vertex_input_t* vertex = &ctx->d3d.vertex_input;
    size_t              total  = EVAL_MIN(((op & 0xFF) - 3) / 4, XE2_SHADER_MAX_BINDINGS);

    // BUG: XE2_SHADER_MAX_BINDINGS value is wrong. We could have huge
    //      vertex buffers of sizes:
    //        Buffer Starting Address: 0xfffffffefe83f090
    //      0xfffffffefff75c8c: 0x001c0f70: Dword 3
    //        Buffer Size : 1838960 vertex buffer 0, size 2097152
    //        -0.21       0.57       0.01
    //        -0.20       0.59       0.01
    //        -0.16       0.56      -0.02
    for (size_t i = 0; i < total; ++i) {
        uint32_t buf[4] = {0};
        xe2_dma_read_many(xe2, xe2_dma_offset(ring, (1 + i * 4) * 4), buf, 4);

        uint32_t binding = (buf[0] >> 26) & 0x3F;
        if (binding >= XE2_SHADER_MAX_BINDINGS) {
            continue;
        }
        vertex->buffer[binding].stride = (buf[0] >> 0) & 0xFFF;
        vertex->buffer[binding].addr   = xe2_ppgtt_translate(xe2, pdp4, xe2_addr_63_6_mask(buf[1], buf[2]));
        vertex->buffer[binding].size   = buf[3];
        if (binding + 1 > vertex->buffer_count) {
            vertex->buffer_count = binding + 1;
        }

        // Print first submitted vertices to prove it works. Since big
        // amount of them is passed, we should consider deferring access
        // to them somewhere near Vulkan renderer.
        {
            uint32_t buffers[16] = {0};
            xe2_dma_read_many(xe2, vertex->buffer[binding].addr, buffers, STATIC_ARRAY_SIZE(buffers));
            for (uint32_t j = 0; j + 2 < STATIC_ARRAY_SIZE(buffers); j += 3) {
                float x, y, z;
                memcpy(&x, &buffers[j + 0], sizeof(x));
                memcpy(&y, &buffers[j + 1], sizeof(y));
                memcpy(&z, &buffers[j + 2], sizeof(z));
                char dump[128] = {0};
                sprintf(dump, "Vertex[%u]: %.3f %.3f %.3f", j / 3, (double)x, (double)y, (double)z);
                rvvm_info("%s", dump);
            }
        }
    }

    ctx->d3d.ff_dirty = 1;
}

static inline void xe2_3dstate_parse_binding_table_entry(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, rvvm_addr_t pdp4,
                                                         xe2_dma_addr_t dma, xe2_shader_kind_t kind)
{
    uint32_t payload[14] = {0};
    xe2_dma_read_many(xe2, dma, payload, STATIC_ARRAY_SIZE(payload));
    for (size_t i = 0; i < STATIC_ARRAY_SIZE(payload); ++i) {
        rvvm_info("Binding table pointers [%zu]: 0x%x", i, payload[i]);
    }

    // Temporary zero index.
    xe2_surface_state_t* surface = &ctx->d3d.surface[kind][0];

    surface->isl_format = xe2_reg_field_get(xe2_reg_genmask(26, 18), payload[0]);
    surface->tile_mode  = xe2_reg_field_get(xe2_reg_genmask(13, 12), payload[0]);
    surface->width      = xe2_reg_field_get(xe2_reg_genmask(13, 0), payload[2]);
    surface->height     = xe2_reg_field_get(xe2_reg_genmask(29, 16), payload[2]);
    surface->pitch      = xe2_reg_field_get(xe2_reg_genmask(17, 0), payload[3]);
    surface->base       = xe2_ppgtt_translate(xe2, pdp4, xe2_concat_lohi(payload[8], payload[9]));
    surface->valid      = !!surface->base.addr;
}

// This suspicious:
//
// We have updated binding table base and for some reason
// translation logic suddenly fucked up.
//
// Binding table pointers (XE2_SHADER_VS): Offset: 0
// Binding table pointers (XE2_SHADER_VS): Base: 13f600000
// Binding table pointers (XE2_SHADER_VS): Translated entry: 0x400000
// Binding table pointers (XE2_SHADER_VS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_HS): Offset: 0
// Binding table pointers (XE2_SHADER_HS): Base: 13f600000
// Binding table pointers (XE2_SHADER_HS): Translated entry: 0x400000
// Binding table pointers (XE2_SHADER_HS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_DS): Offset: 0
// Binding table pointers (XE2_SHADER_DS): Base: 13f600000
// Binding table pointers (XE2_SHADER_DS): Translated entry: 0x400000
// Binding table pointers (XE2_SHADER_DS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_GS): Offset: 0
// Binding table pointers (XE2_SHADER_GS): Base: 13f600000
// Binding table pointers (XE2_SHADER_GS): Translated entry: 0x400000
// Binding table pointers (XE2_SHADER_GS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_PS): Offset: 20
// Binding table pointers (XE2_SHADER_PS): Base: 13f600000
// Binding table pointers (XE2_SHADER_PS): Translated entry: 0x400020
// Binding table pointers (XE2_SHADER_PS): Payload DMA 0x170080
// Binding table pointers (XE2_SHADER_PS): Count: 1
// Binding table pointers (XE2_SHADER_PS): 0x20 (raw: 0x20)
// Binding table pointers [0]: 0x3301c000 << Correct
// Binding table pointers [1]: 0x200010e
// Binding table pointers [2]: 0x437077f
// Binding table pointers [3]: 0x1dff
// Binding table pointers [4]: 0x0
// Binding table pointers [5]: 0x20100
// Binding table pointers [6]: 0x0
// Binding table pointers [7]: 0x9770000
// Binding table pointers [8]: 0xff600000
// Binding table pointers [9]: 0xfffffffe
// Binding table pointers [10]: 0x0
// Binding table pointers [11]: 0x0
// Binding table pointers [12]: 0x0
// Binding table pointers [13]: 0x0
//
// Binding table pointers (XE2_SHADER_VS): Offset: 0
// Binding table pointers (XE2_SHADER_VS): Base: 13f400000
// Binding table pointers (XE2_SHADER_VS): Translated entry: 0x1c00000
// Binding table pointers (XE2_SHADER_VS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_HS): Offset: 0
// Binding table pointers (XE2_SHADER_HS): Base: 13f400000
// Binding table pointers (XE2_SHADER_HS): Translated entry: 0x1c00000
// Binding table pointers (XE2_SHADER_HS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_DS): Offset: 0
// Binding table pointers (XE2_SHADER_DS): Base: 13f400000
// Binding table pointers (XE2_SHADER_DS): Translated entry: 0x1c00000
// Binding table pointers (XE2_SHADER_DS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_GS): Offset: 0
// Binding table pointers (XE2_SHADER_GS): Base: 13f400000
// Binding table pointers (XE2_SHADER_GS): Translated entry: 0x1c00000
// Binding table pointers (XE2_SHADER_GS): Payload DMA 0x0
// Binding table pointers (XE2_SHADER_PS): Offset: 20
// Binding table pointers (XE2_SHADER_PS): Base: 13f400000
// Binding table pointers (XE2_SHADER_PS): Translated entry: 0x1c00020
// Binding table pointers (XE2_SHADER_PS): Payload DMA 0x80 <<< BUG: This
// Binding table pointers (XE2_SHADER_PS): Count: 1
// Binding table pointers (XE2_SHADER_PS): 0x20 (raw: 0x20)
// Binding table pointers [0]: 0x15040001
// Binding table pointers [1]: 0x600
// Binding table pointers [2]: 0x6210
// Binding table pointers [3]: 0x11080005
// Binding table pointers [4]: 0x600
// Binding table pointers [5]: 0x0
// Binding table pointers [6]: 0x608
// Binding table pointers [7]: 0x0
// Binding table pointers [8]: 0x610
// Binding table pointers [9]: 0x0
// Binding table pointers [10]: 0x78150009
// Binding table pointers [11]: 0x0
// Binding table pointers [12]: 0x0
// Binding table pointers [13]: 0x0
static inline void xe2_ring_3dstate_binding_table_pointers_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, rvvm_addr_t pdp4,
                                                               xe2_dma_addr_t ring, uint32_t op)
{
    xe2_shader_kind_t kind   = xe2_binding_table_cmd_to_stage(XE2_GFXPIPE_OPCODES_MASKED(op));
    const char*       name   = xe2_shader_kind_to_string(kind);
    uint32_t          cmd[2] = {0};
    xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));

    ctx->d3d.binding_table_offset[kind] = cmd[1] & xe2_reg_genmask(20, 5);

    rvvm_info("Binding table pointers (%s): Offset: %x", name, ctx->d3d.binding_table_offset[kind]);
    rvvm_info("Binding table pointers (%s): Base: %lx", name, ctx->addr_binding_table_base);

    rvvm_addr_t    binding_table_address = ctx->addr_binding_table_base + ctx->d3d.binding_table_offset[kind];
    xe2_dma_addr_t binding_table         = xe2_ppgtt_translate(xe2, pdp4, binding_table_address);
    uint32_t       binding_table_entry   = xe2_dma_read_32(xe2, binding_table, 0);

    rvvm_info("Binding table pointers (%s): Translated entry: 0x%lx", name, binding_table.addr);

    xe2_dma_addr_t entry_dma = xe2_ppgtt_translate(xe2, pdp4, ctx->addr_surf_state + binding_table_entry);
    rvvm_info("Binding table pointers (%s): Payload DMA 0x%lx", name, entry_dma.addr);

    // Guest sometimes reports zeroed 3DSTATE commands for some reason. If so,
    // skip them.
    if (unlikely(entry_dma.addr == 0x00)) {
        return;
    }

    rvvm_info("Binding table pointers (%s): Count: %u", name, ctx->d3d.binding_table_entry_count[kind]);
    if (ctx->d3d.binding_table_entry_count[kind] > 0) {
        rvvm_info("Binding table pointers (%s): 0x%x (raw: 0x%x)", name, ctx->d3d.binding_table_offset[kind], cmd[1]);

        xe2_3dstate_parse_binding_table_entry(xe2, ctx, pdp4, entry_dma, kind);
        xe2_surface_print(name, &ctx->d3d.surface[kind][0]);
    }
}

static inline void xe2_3dstate_base_address_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_dma_addr_t ring)
{
    // discard flags (MOCS, modify, enable).
    //
    // I assume, these addresses are per-LRC. So we need to store them
    // accordingly. Store XE2_MAX_CONTEXTS addresses rather senseless,
    // find optimized approach.
    uint32_t cmd[21] = {0};
    xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
    ctx->addr_general_state    = xe2_addr_63_12_mask(cmd[1], cmd[2]);
    ctx->addr_surf_state       = xe2_addr_63_12_mask(cmd[4], cmd[5]);
    ctx->addr_dynamic_state    = xe2_addr_63_12_mask(cmd[6], cmd[7]);
    ctx->addr_indirect_object  = xe2_addr_63_12_mask(cmd[8], cmd[9]);
    ctx->addr_instr            = xe2_addr_63_12_mask(cmd[10], cmd[11]);
    ctx->addr_bindless_surface = xe2_addr_63_12_mask(cmd[16], cmd[17]);
    ctx->addr_bindless_sampler = xe2_addr_63_12_mask(cmd[19], cmd[20]);
}

// The supplied ring DMA address is normalized such that the first dword is the
// currently processed instruction header (opcode).
static inline uint32_t xe2_gfxpipe_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_dma_addr_t ring, rvvm_addr_t pdp4,
                                       uint32_t op)
{
    switch (XE2_GFXPIPE_OPCODES_MASKED(op)) {
        case XE2_GFXPIPE_CMD_PIPE_CONTROL:
            xe2_gfxpipe_pipe_control_cmd(xe2, ring);
            break;

        case XE2_GFXPIPE_CMD_STATE_BASE_ADDRESS:
            xe2_3dstate_base_address_cmd(xe2, ctx, ring);
            break;

        case XE2_GFXPIPE_CMD_3DSTATE_PS:
            xe2_3dstate_ps_cmd(xe2, ctx, pdp4, ring);
            break;
        case XE2_GFXPIPE_CMD_3DSTATE_VS:
            xe2_3dstate_vs_cmd(xe2, ctx, pdp4, ring);
            break;
        case XE2_GFXPIPE_CMD_3DSTATE_GS:
            xe2_3dstate_gs_cmd(xe2, ctx, pdp4, ring);
            break;
        case XE2_GFXPIPE_CMD_3DSTATE_HS:
            xe2_3dstate_hs_cmd(xe2, ctx, pdp4, ring);
            break;

        case XE2_GFXPIPE_CMD_3DSTATE_VERTEX_BUFFERS:
            xe2_3dstate_vertex_buffers_cmd(xe2, ctx, ring, pdp4, op);
            break;

        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_VS:
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_HS:
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_DS:
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_GS:
        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POINTERS_PS:
            xe2_ring_3dstate_binding_table_pointers_cmd(xe2, ctx, pdp4, ring, op);
            break;

        case XE2_GFXPIPE_CMD_3DSTATE_BINDING_TABLE_POOL_ALLOC: {
            uint32_t cmd[4] = {0};
            xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
            ctx->addr_binding_table_base = xe2_addr_63_12_mask(cmd[1], cmd[2]);
            rvvm_info("BINDING_TABLE_POOL_ALLOC base: 0x%lx", ctx->addr_binding_table_base);
            break;
        }
        // Unused in glmark2-es2-drm.
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_VS:
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_HS:
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_DS:
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_GS:
        case XE2_GFXPIPE_CMD_3DSTATE_CONSTANT_PS: {
            xe2_shader_kind_t kind = xe2_constant_cmd_to_stage(XE2_GFXPIPE_OPCODES_MASKED(op));

            uint32_t cmd[XE2_CONST_CMD_DWORDS] = {0};
            xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
            xe2_3dstate_constant(xe2, ctx, kind, pdp4, cmd);
            break;
        }

        case XE2_GFXPIPE_CMD_3DSTATE_VF_TOPOLOGY: {
            uint32_t cmd[2] = {0};
            xe2_dma_read_many(xe2, ring, cmd, 2);
            ctx->d3d.vertex_input.topology = cmd[1] & 0x3F; // PRIM_TOPOLOGY_TYPE enum
            ctx->d3d.ff_dirty              = true;
            break;
        }
        case XE2_GFXPIPE_CMD_3DPRIMITIVE: {
            uint32_t cmd[7] = {0};
            xe2_dma_read_many(xe2, ring, cmd, STATIC_ARRAY_SIZE(cmd));
            xe2_3dprimitive(xe2, ctx, cmd);
            break;
        }
        default:
            break;
    }

    UNUSED(pdp4);
    return (op & 0xFF) + 2;
}

static uint32_t xe2_ring_cmd(xe2_dev_t* xe2, xe2_submit_ctx_t* ctx, xe2_dma_addr_t ring, rvvm_addr_t pdp4, uint32_t op,
                             bool* user_int)
{
    switch (XE2_INSTR_TYPE(op)) {
        case XE2_INSTR_TYPE_MI:
            return xe2_mi_cmd(xe2, ctx, ring, pdp4, op, user_int);
            break;
        case XE2_INSTR_TYPE_GFXPIPE:
            return xe2_gfxpipe_cmd(xe2, ctx, ring, pdp4, op);
            break;
        case XE2_INSTR_TYPE_GSC:
            return (op & 0xFF) + 2;
        case XE2_INSTR_TYPE_RESOURCE_BARRIER:
            // This instruction always has size equals 3 bytes per Mesa.
            return 3;
        default:
            rvvm_fatal("Unknown instruction type: %u", XE2_INSTR_TYPE(op));
            return 0;
    }
}

// Walk the LRC ring between HEAD and TAIL and execute the post-sync seqno
// stores the job appended (MI_STORE_DATA_IMM / MI_FLUSH_DW / PIPE_CONTROL with a
// GGTT write). We do not run the batch itself; only its completion postamble has
// observable side effects the driver waits on (the seqno reaching the fence
// value). Returns true if a user interrupt was found in the stream.
static bool xe2_ring_replay(xe2_dev_t* xe2, uint32_t context_idx)
{
    xe2_submit_ctx_t* ctx = &xe2->ctx[context_idx];

    uint32_t    ring_ggtt = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_RING_START);
    uint32_t    head      = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_RING_HEAD);
    uint32_t    tail      = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_RING_TAIL);
    uint32_t    ctl       = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_RING_CTL);
    uint32_t    ppgtt_lo  = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_RING_PDP0_LDW);
    uint32_t    ppgtt_hi  = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_RING_PDP0_UDW);
    rvvm_addr_t pdp4      = xe2_concat_lohi(ppgtt_lo, ppgtt_hi);

    if (unlikely(ring_ggtt == 0 || head == tail)) {
        return false;
    }

    xe2_dma_addr_t ring = xe2_ggtt_translate(xe2, ring_ggtt);
    if (unlikely(ring.addr == 0)) {
        return false;
    }

    // RING_CTL holds (size - PAGE_SIZE) page-aligned; recover the dword count.
    uint32_t ring_bytes = (ctl & 0x003FF000U) + 0x1000U;
    uint32_t ring_dw    = ring_bytes / 4;
    bool     user_int   = false;

    uint32_t i   = (head / 4) % ring_dw;
    uint32_t end = (tail / 4) % ring_dw;

    for (uint32_t guard = 0; i != end && guard < ring_dw; guard++) {
        uint32_t op = xe2_dma_read_32(xe2, ring, i * 4);
        // rvvm_info("(GGTT) Dequeued opcode: 0x%x, instruction type: 0x%x, size %u/%u", op, XE2_INSTR_TYPE(op), guard,
        //           ring_dw);

        // How many bytes was consumed by incoming command. That far we
        // will go over the buffer in the next iteration.
        uint32_t len = xe2_ring_cmd(xe2, ctx, xe2_dma_offset(ring, i * 4), pdp4, op, &user_int);

        i = (i + len) % ring_dw;
    }

    // The ring is now drained; advance HEAD so the next submission is isolated.
    xe2_lrc_reg_write(xe2, ctx, XE2_CTX_RING_HEAD, tail);

    return user_int;
}

// -----------------------------------------------------------
// Context registration & processing
// -----------------------------------------------------------



// Complete render-engine work. Replay the ring's seqno stores, then post an rcs0
// user interrupt via the memory-based interrupt pages whose GGTT locations the
// driver baked into the LRC register state, so it wakes, reads the seqno and
// signals the job fence.
static void xe2_signal_job_completion(xe2_dev_t* xe2, size_t context_idx)
{
    xe2_submit_ctx_t* ctx = &xe2->ctx[context_idx];

    if (unlikely(ctx->pphwsp.addr == 0)) {
        return;
    }

    if (!xe2_ring_replay(xe2, context_idx)) {
        return;
    }

    xe2_lrc_reg_write(xe2, ctx, XE2_LRC_SEQNO_PPHWSP_OFFSET / 4, ctx->seqno++);
    ++ctx->seqno;

    uint32_t src_ggtt = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_INT_SRC_REPORT_PTR);
    uint32_t sts_ggtt = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_INT_STATUS_REPORT_PTR);

    if (src_ggtt && sts_ggtt) {
        // Render source byte: its IIR bit in the engine's source-report page.
        xe2_dma_addr_t src = xe2_ggtt_translate(xe2, src_ggtt);
        xe2_dma_write_32(xe2, src, XE2_MEMIRQ_RENDER_SRC_BYTE, XE2_MEMIRQ_BYTE_SET);

        // Render user-interrupt byte: byte 0 of the engine's status vector.
        xe2_dma_addr_t sts = xe2_ggtt_translate(xe2, sts_ggtt);
        xe2_dma_write_32(xe2, sts, XE2_MEMIRQ_RENDER_STATUS_BYTE, XE2_MEMIRQ_BYTE_SET);
    }

    // The engine reports on its own MSI-X vector, which the driver recorded in
    // the context; the GuC owns vector 0, so raising 0 here would be ignored.
    uint32_t msix_vec = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_CS_INT_VEC_DATA) & 0xFFFF;
    rvvm_pci_send_irq(xe2->pci_func, msix_vec);
}

// Record (or look up) a submission context by its PPHWSP address. A freshly
// registered context baselines last_tail to its current ring tail so any ring
// content present at registration (priming, wa_bb setup) is not replayed as a
// job; only tail advances past this baseline are treated as submissions.
//
// Note that we don't strictly need to initialize seqno, since driver
// could start with any number if we increment its value.
static xe2_submit_ctx_t* xe2_track_context(xe2_dev_t* xe2, xe2_dma_addr_t pphwsp)
{
    int free_slot = -1;
    for (size_t i = 0; i < XE2_MAX_CONTEXTS; i++) {
        if (xe2->ctx[i].valid && xe2->ctx[i].pphwsp.addr == pphwsp.addr) {
            // Re-registration: re-baseline so stale ring content is ignored.
            xe2->ctx[i].pphwsp    = pphwsp;
            xe2->ctx[i].last_tail = xe2_lrc_reg_read(xe2, &xe2->ctx[i], XE2_CTX_RING_TAIL);
            return &xe2->ctx[i];
        }
        if (free_slot < 0 && !xe2->ctx[i].valid) {
            free_slot = (int)i;
        }
    }
    if (free_slot >= 0) {
        xe2->ctx[free_slot].pphwsp    = pphwsp;
        xe2->ctx[free_slot].last_tail = xe2_lrc_reg_read(xe2, &xe2->ctx[free_slot], XE2_CTX_RING_TAIL);
        xe2->ctx[free_slot].valid     = true;
    } else {
        rvvm_warn("All hardware engine slots are busy");
        free_slot = 0;
    }
    return &xe2->ctx[free_slot];
}

// Scan every registered context and complete those whose ring tail advanced
// since we last serviced them. This is the genuine "job submitted" signal and
// is independent of whether the triggering doorbell also carried a CT message,
// so concurrent CT exchanges (e.g. GuC opt-in) are never perturbed.
static void xe2_complete_advanced_contexts(xe2_dev_t* xe2)
{
    for (size_t i = 0; i < XE2_MAX_CONTEXTS; i++) {
        if (!xe2->ctx[i].valid) {
            continue;
        }
        uint32_t tail = xe2_lrc_reg_read(xe2, &xe2->ctx[i], XE2_CTX_RING_TAIL);
        if (tail == xe2->ctx[i].last_tail) {
            continue;
        }
        xe2_signal_job_completion(xe2, i);
        xe2->ctx[i].last_tail = tail;
    }
}

// Publish the SLPC shared-data state the driver polls during GuC-PC start: the
// running global state plus the unslice frequency caps. Without this the start
// handshake spins on global_state and times out, disabling dynamic frequency
// control and failing probe.
static void xe2_slpc_publish(xe2_dev_t* xe2)
{
    if (!xe2->guc.slpc_data_valid) {
        return;
    }
    uint32_t freq = xe2_reg_field_prep(XE2_SLPC_FREQ_MAX_UNSLICE_MASK, XE2_GT_FREQ_RP0_RATIO)
                  | xe2_reg_field_prep(XE2_SLPC_FREQ_MIN_UNSLICE_MASK, XE2_GT_FREQ_RPN_RATIO);
    xe2_dma_write_32(xe2, xe2->guc.slpc_data_addr, XE2_SLPC_OFF_HEADER_SIZE, XE2_SLPC_SHARED_DATA_SIZE);
    xe2_dma_write_32(xe2, xe2->guc.slpc_data_addr, XE2_SLPC_OFF_GLOBAL_STATE, XE2_SLPC_GLOBAL_STATE_RUNNING);
    xe2_dma_write_32(xe2, xe2->guc.slpc_data_addr, XE2_SLPC_OFF_TASK_STATE_FREQ, freq);
}

// Handle an SLPC request sub-event. The reset/query events carry the shared-data
// BO address in the first argument; parameter set/unset carry an id (+value).
// Every variant publishes the running state so the driver's poll succeeds.
//
// Noticed difference in behaviour of following programs
// 1. drm_red: few lines of code painting screen with red color
// 2. glmark2-es2-drm: not works, polls on XE2_GUC_ACTION_SCHED_CONTEXT_MODE_SET.
//
// drm_red does not interact with GuC action 0x3003 (XE2_GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST),
// whereas OpenGL renderer does. Maybe, driver expects something and waits for event by polling.
static void xe2_slpc_request(xe2_dev_t* xe2, const uint32_t* msg)
{
    uint32_t event = xe2_reg_field_get(XE2_SLPC_EVENT_ID_MASK, msg[1]);

    switch (event) {
        case XE2_SLPC_EVENT_RESET:
        case XE2_SLPC_EVENT_QUERY_TASK_STATE: {
            rvvm_addr_t bo           = xe2_concat_lohi(msg[2], msg[3]);
            xe2->guc.slpc_data_addr  = xe2_ggtt_translate(xe2, bo);
            xe2->guc.slpc_data_valid = true;
            break;
        }
        default:
            // Parameter set/unset and other events keep the existing BO.
            break;
    }

    xe2_slpc_publish(xe2);
}

// GuC Command Transport: the driver writes H2G requests into a circular ring
// and, for blocking sends, waits for a G2H response that echoes the request's
// fence. Drain every pending H2G message, act on it, and push a fence-matched
// success response for each request.
static void xe2_guc_host_interrupt(xe2_dev_t* xe2)
{
    // Scratch-register (MMIO) transport path, used before the CT rings are up.
    xe2_guc_action(xe2, xe2->guc.actions_h2g, xe2->guc.actions_g2h);

    uint32_t dwords = xe2->guc.ctb_h2g_size / 4;
    if (dwords == 0) {
        return;
    }

    uint32_t head = xe2_dma_read_32(xe2, xe2->guc.ctb_h2g_descriptor_addr, 0);
    uint32_t tail = xe2_dma_read_32(xe2, xe2->guc.ctb_h2g_descriptor_addr, 4);

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

        // rvvm_info("GuC CT: action=0x%x type=%u fence=0x%x (%u) dwords=%u", action, type, fence, fence, num_dwords);

        switch (action) {
            // Note that context ID = GuC ID.
            case XE2_GUC_ACTION_REGISTER_CONTEXT: {
                // The registered context address points at the per-process HW
                // status page (PPHWSP), which is the first page of the context.
                rvvm_addr_t hwlrca  = xe2_concat_lohi(msg[10], msg[11]);
                hwlrca             &= 0x0000FFFFFFFFF000ULL; // page addr only; drop desc flags + engine class/instance
                xe2_dma_addr_t hwlrca_dma = xe2_ggtt_translate(xe2, hwlrca);
                // Begin tracking this context for tail-advance completion.
                xe2_submit_ctx_t* ctx = xe2_track_context(xe2, hwlrca_dma);
                // The first registered context is rcs0 (irq_page 0); its source
                // pointer reveals the shared memirq BO base for GuC signalling.
                if (xe2->guc.memirq_base_ggtt == 0) {
                    uint32_t src = xe2_lrc_reg_read(xe2, ctx, XE2_CTX_INT_SRC_REPORT_PTR);
                    if (src > XE2_MEMIRQ_SOURCE_PAGE_OFFSET) {
                        xe2->guc.memirq_base_ggtt = src - XE2_MEMIRQ_SOURCE_PAGE_OFFSET;
                    }
                }
                break;
            }
            // A brushstroke liturgy, the first of three offerings to be
            // conjured as tribute to the renderer.
            case XE2_GUC_ACTION_DEREGISTER_CONTEXT: {
                uint32_t done = msg[1]; // GuC ID.
                xe2_guc_g2h_event(xe2, XE2_GUC_ACTION_DEREGISTER_CONTEXT_DONE, &done, 1);
                break;
            }
            // Note that driver expects no response on similar request
            // XE2_GUC_ACTION_SCHED_CONTEXT
            case XE2_GUC_ACTION_SCHED_CONTEXT_MODE_SET: {
                // The driver enables (or disables) scheduling on a context and
                // blocks on a matching SCHED_CONTEXT_MODE_DONE event before it
                // can submit or tear down. msg[1] = guc_id, msg[2] = runnable
                // state (enable/disable); echo both back so its pending_enable/
                // pending_disable wait clears and the reserved G2H space frees.
                //
                // This called only when context was not registered yet.
                uint32_t done[2] = {msg[1], msg[2]};
                rvvm_info("GuC SCHED_CONTEXT_MODE_SET: %s", msg[2] ? "enable" : "disable");
                xe2_guc_g2h_event(xe2, XE2_GUC_ACTION_SCHED_CONTEXT_MODE_DONE, done, 2);
                break;
            }
            case XE2_GUC_ACTION_HOST2GUC_PC_SLPC_REQUEST:
                // Bring up GuC-PC: publish the running SLPC state and frequency
                // caps into the shared BO so the driver's start handshake clears.
                xe2_slpc_request(xe2, msg);
                break;
            case XE2_GUC_ACTION_AUTHENTICATE_HUC:
                // The GuC verifies the HuC firmware image; report success so the
                // driver's HUC_KERNEL_LOAD_INFO poll sees the firmware verified.
                xe2->guc.huc_authenticated = true;
                break;
            case XE2_GUC_ACTION_TLB_INVALIDATION:
            case XE2_GUC_ACTION_TLB_INVALIDATION_ALL: {
                // Report completed invalidation without meaningful work.
                //
                // Bug: Look at invalidation seqno's:
                // info: XE2_GUC_ACTION_TLB_INVALIDATION_(ALL?), seqno: 228
                // info: XE2_GUC_ACTION_TLB_INVALIDATION_(ALL?), seqno: 229
                // [   67.551720] xe 0000:00:01.0: [drm] Tile0: GT0: Context scheduled
                // [   67.556618] xe 0000:00:01.0: [drm] Tile0: GT0: Set PPGTT: 0x10b01b
                // info: XE2_GUC_ACTION_TLB_INVALIDATION_(ALL?), seqno: 230
                // [   67.567649] xe 0000:00:01.0: [drm] Tile0: GT0: Set PPGTT: 0x10b01b
                // [   67.572331] xe 0000:00:01.0: [drm] Tile0: GT0: Set PPGTT: 0x10b01b
                // [   67.577021] xe_sched_job_arm: Assign job seqno: 4294967290
                // [   67.580633] emit_copy_timestamp: dw[0] (cmd):      0x12480002
                // info: (PPGTT) ... Done, moved 0 bytes
                // info: (PPGTT) ... Done, moved 7 bytes
                //
                // ... But! When Vulkan rendering is submitted, it probably blocks
                // MMIO requests submission:
                // [   68.354120] xe_sched_job_arm: Assign job seqno: 4294967178
                // [   68.364864] xe_sched_job_arm: Assign job seqno: 4294967170
                // [   70.801727] xe 0000:00:01.0: [drm] *ERROR* TLB invalidation fence timeout, seqno=231 recv=230
                // [   73.361299] xe 0000:00:01.0: [drm] *ERROR* TLB invalidation fence timeout, seqno=232 recv=230
                // [   75.665224] xe 0000:00:01.0: [drm] *ERROR* TLB invalidation fence timeout, seqno=233 recv=230
                // [   77.969124] xe 0000:00:01.0: [drm] *ERROR* TLB invalidation fence timeout, seqno=234 recv=230
                // [   80.273245] xe 0000:00:01.0: [drm] *ERROR* TLB invalidation fence timeout, seqno=235 recv=230
                // ...
                uint32_t seqno = msg[1];
                // rvvm_info("XE2_GUC_ACTION_TLB_INVALIDATION_(ALL?), seqno: %u", seqno);
                xe2_guc_g2h_event(xe2, XE2_GUC_ACTION_TLB_INVALIDATION_DONE, &seqno, 1);
                break;
            }
            default:
                break;
        }

        // A request expects a fence-matched response; an event does not.
        // When XE2_GUC_HXG_TYPE_FAST_REQUEST is used, driver expects no
        // response.
        if (type == XE2_GUC_HXG_TYPE_REQUEST) {
            xe2_guc_g2h_response(xe2, fence, 0);
        }

        head = (head + XE2_GUC_CTB_HDR_LEN + num_dwords) % dwords;
    }

    // Publish the updated consumer head.
    xe2_dma_write_32(xe2, xe2->guc.ctb_h2g_descriptor_addr, 0, head);

    // The job-submission and CT-message doorbells share register 0x1901F0. Rather
    // than gate on the presence of CT traffic (which also rides submission
    // doorbells and would suppress real completions), complete a context only
    // when its LRC ring tail has advanced past what we last serviced. This is the
    // true submission signal and leaves concurrent CT exchanges untouched.
    xe2_complete_advanced_contexts(xe2);
}

// -----------------------------------------------------------
// MMIO read/write handlers
// -----------------------------------------------------------



static inline bool xe2_skip_mmio_range(size_t offset)
{
    return 1;
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
    skip |= offset == XE2_REG_PRIMARY_SPI_ADDRESS;
    skip |= offset == XE2_REG_PRIMARY_SPI_TRIGGER;
    skip |= offset == XE2_REG_GT_FORCEWAKE_GT;
    skip |= offset == XE2_REG_GT_FORCEWAKE_ACK_GT_MTL;
    skip |= offset == XE2_REG_PCH_PP_STATUS;
    skip |= offset == 0xB404;
    skip |= offset == 0x70000;
    return skip;
}

// Map a DMC register-window offset to its backing shadow byte, or NULL if the
// offset lies outside both windows. Used so DMC loader writes read back.
static inline uint8_t* xe2_dmc_shadow(xe2_dev_t* xe2, size_t offset)
{
    if (offset >= 0x5F000 && offset < sizeof(xe2->dmc_mmio_5f) + 0x5F000) {
        return &xe2->dmc_mmio_5f[offset - 0x5F000];
    }
    if (offset >= 0x8F000 && offset < sizeof(xe2->dmc_mmio_8f) + 0x8F000) {
        return &xe2->dmc_mmio_8f[offset - 0x8F000];
    }
    return NULL;
}

static void xe2_mmio_read(rvvm_reg_dev_t* dev, void* data, size_t size, size_t offset)
{
    UNUSED(size);

    xe2_dev_t* xe2 = rvvm_region_data(dev);
    spin_lock(&xe2->lock);

    if (!xe2_skip_mmio_range(offset)) {
        rvvm_info("PCI read: offset=%zx, data=%x", offset, read_uint32_le(data));
    }

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
            uint32_t cmd  = xe2_reg_field_prep(XE2_REG_PCODE_MAILBOX_READY_MASK, 0);
            cmd          |= 0x0;
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

        // GT frequency caps and status. The driver decodes RP0/RPn/RPe and the
        // current/requested ratios from these to bring up dynamic frequency
        // control; reporting non-zero ratios keeps freq management enabled.
        case XE2_REG_RP_STATE_CAP: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_RP_STATE_CAP_RP0_MASK, XE2_GT_FREQ_RP0_RATIO)
                         | xe2_reg_field_prep(XE2_REG_RP_STATE_CAP_RPN_MASK, XE2_GT_FREQ_RPN_RATIO);
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GT_RPE_FREQUENCY:
            write_uint32_le(data, xe2_reg_field_prep(XE2_REG_GT_RPE_FREQUENCY_RPE_MASK, XE2_GT_FREQ_RPE_RATIO));
            break;
        case XE2_REG_GT_PERF_STATUS:
            write_uint32_le(data, xe2_reg_field_prep(XE2_REG_GT_PERF_STATUS_CAGF_MASK, XE2_GT_FREQ_RPE_RATIO));
            break;
        case XE2_REG_RPNSWREQ:
            write_uint32_le(data, xe2_reg_field_prep(XE2_REG_RPNSWREQ_RATIO_MASK, XE2_GT_FREQ_RPE_RATIO));
            break;

        case XE2_REG_HUC_KERNEL_LOAD_INFO:
            write_uint32_le(data, xe2->guc.huc_authenticated ? XE2_REG_HUC_KERNEL_LOAD_INFO_SUCCESSFUL : 0);
            break;

        case XE2_REG_PRIMARY_SPI_TRIGGER:
            write_uint32_le(data, xe2_spi_read32(xe2));
            break;
        case XE2_REG_PRIMARY_SPI_ADDRESS:
            write_uint32_le(data, xe2->spi_address);
            break;
        case XE2_REG_SPI_STATIC_REGIONS:
            // Static region id the driver echoes back when selecting the region.
            write_uint32_le(data, 0);
            break;
        case XE2_REG_OROM_OFFSET:
            // Option-ROM base within flash; our VBT image sits at offset 0.
            write_uint32_le(data, 0);
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
            uint32_t cmd = xe2_reg_field_prep(xe2_reg_genmask(14, 8), 0x40) // Tile size
                         | xe2_reg_field_prep(xe2_reg_genmask(7, 1), 0x00); // Tile offset
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
            write_uint32_le(data, xe2->pp.status);
            break;
        case XE2_REG_PP_CONTROL:
            xe2->pp.control |= xe2_reg_field_prep(XE2_REG_PP_CONTROL_POWER_ON_MASK, 1)
                             | xe2_reg_field_prep(XE2_REG_PP_CONTROL_EPD_FORCE_VDD_MASK, 1);
            write_uint32_le(data, xe2->pp.control);
            break;
        case XE2_REG_PP_ON_DELAYS:
            write_uint32_le(data, xe2->pp.on_delays);
            break;
        case XE2_REG_PP_OFF_DELAYS:
            write_uint32_le(data, xe2->pp.off_delays);
            break;

        case XE2_REG_PCH_PP_STATUS:
            write_uint32_le(data, xe2->pp_pch.status);
            break;
        case XE2_REG_PCH_PP_CONTROL:
            write_uint32_le(data, xe2->pp_pch.control);
            break;
        case XE2_REG_PCH_PP_ON_DELAYS:
            write_uint32_le(data, xe2->pp_pch.on_delays);
            break;
        case XE2_REG_PCH_PP_OFF_DELAYS:
            write_uint32_le(data, xe2->pp_pch.off_delays);
            break;

        case XE2_REG_HSW_POWER_WELL_CTL1:
        case XE2_REG_HSW_POWER_WELL_CTL2:
        case XE2_REG_HSW_POWER_WELL_CTL3:
        case XE2_REG_HSW_POWER_WELL_CTL4:
            write_uint32_le(data, 0xFFFFFFFF);
            break;

        // Cx0 PHY message bus: return the stored response/command words.
        case XE2_REG_CX0_M2P_MSGBUS_CTL(0):
        case XE2_REG_CX0_M2P_MSGBUS_CTL(1): {
            size_t lane = (offset - XE2_REG_CX0_M2P_MSGBUS_CTL(0)) / 4;
            write_uint32_le(data, xe2->cx0[lane].m2p);
            break;
        }
        case XE2_REG_CX0_P2M_MSGBUS_STATUS(0):
        case XE2_REG_CX0_P2M_MSGBUS_STATUS(1): {
            size_t lane = (offset - XE2_REG_CX0_P2M_MSGBUS_STATUS(0)) / 4;
            write_uint32_le(data, xe2->cx0[lane].p2m);
            break;
        }
        case XE2_REG_XELPDP_PORT_CLOCK_CTL:
            write_uint32_le(data, xe2->port_clock_ctl);
            break;

        // PORT_BUF_CTL1: report the PHY/D2D status the enable sequence polls.
        // SOC PHY is always ready; the D2D link state mirrors its enable bit;
        // the PHY is idle after disable and cannot be idle after enable.
        case XE2_REG_XELPDP_PORT_BUF_CTL1: {
            uint32_t cmd = xe2->port_buf_ctl1;
            if (cmd & XE2_REG_XELPDP_PORT_BUF_CTL1_D2D_LINK_ENABLE_MASK) {
                cmd |= XE2_REG_XELPDP_PORT_BUF_CTL1_D2D_LINK_STATE_MASK;
            } else {
                cmd &= ~XE2_REG_XELPDP_PORT_BUF_CTL1_D2D_LINK_STATE_MASK;
            }

            if (cmd & XE2_REG_XELPDP_PORT_BUF_CTL1_ENABLE) {
                cmd &= ~XE2_REG_XELPDP_PORT_BUF_CTL1_PHY_IDLE_MASK;
            } else {
                cmd |= XE2_REG_XELPDP_PORT_BUF_CTL1_PHY_IDLE_MASK;
            }
            write_uint32_le(data, cmd);
            break;
        }
        // PORT_BUF_CTL2: the lane PHY status mirrors the pipe-reset request (so
        // both the reset-start and reset-end polls observe the transition), and
        // the powerdown-update bits read back cleared (treated as consumed).
        case XE2_REG_XELPDP_PORT_BUF_CTL2: {
            uint32_t cmd = xe2->port_buf_ctl2 & ~XE2_REG_XELPDP_PORT_BUF_CTL2_POWERDOWN_UPDATE_MASK;
            if (xe2->port_buf_ctl2 & XE2_REG_XELPDP_PORT_BUF_CTL2_LANE_PIPE_RESET_MASK) {
                cmd |= XE2_REG_XELPDP_PORT_BUF_CTL2_LANE_PHY_STATUS_MASK;
            } else {
                cmd &= ~XE2_REG_XELPDP_PORT_BUF_CTL2_LANE_PHY_STATUS_MASK;
            }
            write_uint32_le(data, cmd);
            break;
        }

        case XE2_REG_WM_LINETIME_A:
        case XE2_REG_WM_LINETIME_B: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_WM_LINETIME_X_HSW_IPS_LINETIME_MASK, 120)
                         | xe2_reg_field_prep(XE2_REG_WM_LINETIME_X_HSW_LINETIME_MASK, 120);
            write_uint32_le(data, cmd);
            break;
        }

        // Transcoder A enable state: read back what the driver wrote. TRANSCONF
        // also exposes a "state" bit set whenever the transcoder is enabled, so
        // the post-modeset readback sees the pipe as active.
        case XE2_REG_TRANS_DDI_FUNC_CTL_A:
            write_uint32_le(data, xe2->trans_ddi_func_ctl);
            break;
        case XE2_REG_TRANSCONF_A: {
            uint32_t cmd = xe2->transconf;
            if (cmd & XE2_REG_TRANSCONF_ENABLE_MASK) {
                cmd |= XE2_REG_TRANSCONF_STATE_MASK;
            }
            write_uint32_le(data, cmd);
            break;
        }

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
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_GGC_GGMS_MASK, 3) | xe2_reg_field_prep(XE2_REG_GGC_GMS_MASK, 4);
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

        case XE2_REG_GUC_PMTIMESTAMP_LO:
            write_uint32_le(data, 1779018398);
            break;
        case XE2_REG_GUC_PMTIMESTAMP_HI:
            write_uint32_le(data, 0);
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
        case XE2_REG_GUC_DMA_CTRL:
            // START_DMA (bit 0) reads back 0 once the DMA completes.
            write_uint32_le(data, 0);
            break;

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
                xe2->aux[0].ctl |= xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_DONE_MASK, 1);
                xe2->aux[0].ctl |= xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_POWER_REQUEST, 1);
                xe2_emulate_aux_transfer(xe2, 0);
            } else {
                xe2->aux[0].ctl &= ~xe2_reg_field_prep(XE2_REG_DPX_AUX_CH_CTL_POWER_REQUEST, 1);
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

        case XE2_REG_DP_A:
        case XE2_REG_DP_B:
        case XE2_REG_DP_C:
        case XE2_REG_DP_D: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_DP_X_PORT_EN_MASK, 1);
            write_uint32_le(data, cmd);
            break;
        }

        case XE2_REG_TGL_DP_TP_STATUS_A:
            xe2->display.dp_tp_status |= xe2_reg_field_prep(XE2_REG_TGL_DP_TP_STATUS_IDLE_DONE, 1);
            write_uint32_le(data, xe2->display.dp_tp_status);
            break;

        case XE2_DMC_FW_MAIN_OFFSET:          // DMC program offset
        case XE2_DMC_FW_PIPE_A_OFFSET:        // DMC program offset
        case XE2_DMC_FW_PIPE_B_OFFSET:        // DMC program offset
        case XE2_DMC_FW_PIPE_C_OFFSET:        // DMC program offset
        case XE2_DMC_FW_PIPE_D_OFFSET:        // DMC program offset
            write_uint32_le(data, 0xC0A4040); // DMC program size
            break;

        case XE2_REG_DMC_SSP_BASE:
            write_uint32_le(data, xe2->firmware.dmc_base);
            break;

        case XE2_REG_PLANE_CTL_1_A:
        case XE2_REG_PLANE_CTL_1_B: {
            uint32_t cmd = xe2_reg_field_prep(XE2_REG_PLANE_CTL_X_ICL_FORMAT_MASK, 14) // RGB565
                         | xe2_reg_field_prep(XE2_REG_PLANE_CTL_X_KEY_ENABLE_MASK, 1)
                         | xe2_reg_field_prep(XE2_REG_PLANE_CTL_X_ENABLE_MASK, 1);
            write_uint32_le(data, cmd);
            write_uint32_le(data, xe2->display.plane_ctl);
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

        // These assumed to be read-only for guest. When XE2_MI_OP_BATCH_BUFFER_START
        // was issued, driver polls these registers to get some reasonable value.
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

        case XE2_REG_HW_ENGINE_RING_CTL(XE2_HW_ENGINE_RENDER_RING_BASE):
            rvvm_info("Read ring ctl: HW engine renderer");
            write_uint32_le(data, 0);
            break;

        // Top-level display interrupt control. Report enable bit (if set by
        // driver) plus a per-pipe master bit for every pipe with a live IIR.
        case XE2_REG_GEN11_DISPLAY_INT_CTL: {
            uint32_t cmd = xe2->display.int_ctl & XE2_REG_GEN11_DISPLAY_INT_CTL_ENABLE_MASK;
            for (uint32_t pipe = 0; pipe < XE2_PIPE_COUNT; pipe++) {
                if (xe2_display_pipe_live(xe2, pipe)) {
                    cmd |= XE2_REG_GEN11_DISPLAY_INT_CTL_PIPE_MASK(pipe);
                }
            }
            write_uint32_le(data, cmd);
            break;
        }

        // Per-pipe Display-Engine interrupt registers.
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_D):
            write_uint32_le(data, xe2->display.imr[(offset - XE2_REG_DE_PIPE_IMR(0)) / 0x10]);
            break;
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_D):
            write_uint32_le(data, xe2->display.ier[(offset - XE2_REG_DE_PIPE_IER(0)) / 0x10]);
            break;
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_D):
            write_uint32_le(data, xe2->display.isr[(offset - XE2_REG_DE_PIPE_ISR(0)) / 0x10]);
            break;
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_D):
            write_uint32_le(data, xe2->display.iir[(offset - XE2_REG_DE_PIPE_IIR(0)) / 0x10]);
            break;
        case XE2_REG_PIPE_FRMCOUNT(XE2_PIPE_A):
        case XE2_REG_PIPE_FRMCOUNT(XE2_PIPE_B):
        case XE2_REG_PIPE_FRMCOUNT(XE2_PIPE_C):
        case XE2_REG_PIPE_FRMCOUNT(XE2_PIPE_D):
            write_uint32_le(data, xe2->display.frmcount[(offset - XE2_REG_PIPE_FRMCOUNT(0)) / 0x1000]);
            break;

        // GuC-to-host and display interrupt delivery chain (register-based, MSI
        // vector 0). The handler walks DG1_MSTR_TILE_INTR -> GFX_MSTR_IRQ -> the
        // GuC GT path (GT_INTR_DW0 -> INTR_IDENTITY_REG0) or the display path
        // (GEN11_DISPLAY_INT_CTL -> DE_PIPE). Both the GuC latch and a live
        // display pipe contribute to the master bits.
        case XE2_REG_DG1_MSTR_TILE_INTR:
            write_uint32_le(data, (xe2->guc.irq_pending || xe2_display_pending(xe2))
                                      ? (XE2_IRQ_MASTER_BIT | XE2_IRQ_DG1_TILE0_BIT)
                                      : 0);
            break;
        case XE2_REG_GFX_MSTR_IRQ: {
            uint32_t cmd = 0;
            if (xe2->guc.irq_pending || xe2_display_pending(xe2)) {
                cmd |= XE2_IRQ_MASTER_BIT;
            }
            if (xe2->guc.irq_pending) {
                cmd |= XE2_IRQ_GT_DW0_BIT;
            }
            if (xe2_display_pending(xe2)) {
                cmd |= XE2_IRQ_DISPLAY_BIT;
            }
            write_uint32_le(data, cmd);
            break;
        }
        case XE2_REG_GT_INTR_DW0:
            write_uint32_le(data, xe2->guc.irq_pending ? XE2_IRQ_INTR_GUC_BIT : 0);
            break;
        case XE2_REG_GT_INTR_DW1:
            write_uint32_le(data, 0);
            break;
        case XE2_REG_INTR_IDENTITY_REG0: {
            uint32_t ident = 0;
            if (xe2->guc.irq_pending) {
                // intr_vec (bits 15:0) carries the GuC interrupt cause; the GuC
                // handler only queues the G2H worker when GUC_INTR_GUC2HOST is
                // set, so it must be present here.
                ident = XE2_IRQ_INTR_DATA_VALID | (XE2_IRQ_ENGINE_CLASS_OTHER << 16) | (XE2_IRQ_GUC_INSTANCE << 20)
                      | XE2_IRQ_GUC2HOST_VEC;
                // Source consumed; drop the latch so the line goes idle.
                xe2->guc.irq_pending = false;
            }
            write_uint32_le(data, ident);
            break;
        }
        case XE2_REG_INTR_IDENTITY_REG1:
            write_uint32_le(data, 0);
            break;

        default:
            // For safety initialize all unhandled requests to 0.
            // Note that driver expects zero-initialized interrupt registers
            // at startup (ISR/IMR/IIR/IER).
            write_uint32_le(data, 0x0);
            break;
    }

    if (offset >= XE2_DMC_FW_MAIN_OFFSET && offset + 4 <= XE2_DMC_FW_MAIN_OFFSET + sizeof(xe2->firmware.main)) {
        uint32_t word = read_uint32_le(&xe2->firmware.main[offset - XE2_DMC_FW_MAIN_OFFSET]);
        write_uint32_le(data, word);
    }
    if (offset >= 0x60000 && offset + 4 <= 0x62000) {
        uint32_t shadow = xe2->display.pipe_regs_shadow[(offset - 0x60000) / 4];
        rvvm_info("Pipe readback: 0x%zx -> 0x%x", offset, shadow);
        if (shadow) {
            write_uint32_le(data, shadow);
        }
    }
    if (offset >= 0x70000 && offset + 4 <= 0x78000) {
        uint32_t shadow = xe2->display.plane_regs_shadow[(offset - 0x70000) / 4];
        if (shadow) {
            write_uint32_le(data, shadow);
        }
    }

    // Replay the DMC loader's register writes so its post-load verification of
    // each mmio[i] entry matches (avoids the "DMC N mmio[i]/0xADDR incorrect"
    // assertion). These windows hold no other emulated state.
    uint8_t* dmc_shadow = xe2_dmc_shadow(xe2, offset);
    if (dmc_shadow != NULL) {
        write_uint32_le(data, read_uint32_le(dmc_shadow));
    }

    spin_unlock(&xe2->lock);
}

static inline bool xe2_ggtt_mmio_range(size_t offset)
{
    size_t begin = XE2_GGTT_MMIO_BASE;
    size_t end   = XE2_GGTT_MMIO_BASE + XE2_GGTT_MMIO_SIZE;

    return offset >= begin && offset <= end;
}

static void xe2_mmio_write(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t offset)
{
    UNUSED(size);

    xe2_dev_t* xe2 = rvvm_region_data(dev);
    spin_lock(&xe2->lock);

    if (!xe2_skip_mmio_range(offset)) {
        rvvm_info("PCI write: offset=%zx, data=%x, size = %zu", offset, read_uint32_le(data), size);
    }

    if (xe2_ggtt_mmio_range(offset)) {
        xe2_ggtt_mmio_write(xe2, offset, read_uint32_le(data));
        spin_unlock(&xe2->lock);
        return;
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
        case XE2_REG_PRIMARY_SPI_REGIONID:
            // Region selection only; our flash serves a single VBT image.
            break;

        case XE2_REG_DPA_AUX_CH_CTL:
            xe2->aux[0].ctl = read_uint32_le(data);
            break;

        case XE2_REG_DPA_AUX_CH_DATA(0):
        case XE2_REG_DPA_AUX_CH_DATA(1):
        case XE2_REG_DPA_AUX_CH_DATA(2):
        case XE2_REG_DPA_AUX_CH_DATA(3):
        case XE2_REG_DPA_AUX_CH_DATA(4): {
            size_t index             = XE2_REG_DPX_AUX_CH_DATA_INDEX(offset);
            xe2->aux[0].data[index]  = read_uint32_le(data);
            xe2->aux[0].message_size = ((index + 1) * 4);
            break;
        }

        // Top-level display interrupt control: plain read-back storage.
        case XE2_REG_GEN11_DISPLAY_INT_CTL:
            xe2->display.int_ctl = read_uint32_le(data);
            break;

        // Per-pipe Display-Engine interrupt registers.
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_IMR(XE2_PIPE_D):
            xe2->display.imr[(offset - XE2_REG_DE_PIPE_IMR(0)) / 0x10] = read_uint32_le(data);
            break;
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_IER(XE2_PIPE_D):
            xe2->display.ier[(offset - XE2_REG_DE_PIPE_IER(0)) / 0x10] = read_uint32_le(data);
            break;
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_ISR(XE2_PIPE_D):
            xe2->display.isr[(offset - XE2_REG_DE_PIPE_ISR(0)) / 0x10] = read_uint32_le(data);
            break;
        // IIR is write-1-to-clear: clear every written bit.
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_A):
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_B):
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_C):
        case XE2_REG_DE_PIPE_IIR(XE2_PIPE_D):
            xe2->display.iir[(offset - XE2_REG_DE_PIPE_IIR(0)) / 0x10] &= ~read_uint32_le(data);
            break;

        // Capture the pipe-A plane-1 scanout state so the refresh tick can blit
        // the guest framebuffer onto the host window (see xe2_scanout).
        case XE2_REG_PLANE_CTL_1_A:
            xe2->display.plane_ctl = read_uint32_le(data);
            break;
        case XE2_REG_PLANE_STRIDE_1_A:
            xe2->display.plane_stride = read_uint32_le(data);
            break;
        case XE2_REG_PLANE_SIZE_1_A:
            xe2->display.plane_size = read_uint32_le(data);
            break;
        case XE2_REG_PLANE_SURF_1_A:
            xe2->display.plane_surf = read_uint32_le(data);
            break;

        case XE2_REG_TGL_DP_TP_STATUS_A:
            xe2->display.dp_tp_status = read_uint32_le(data);
            break;

        // Cx0 PHY message bus: starting a command runs the transaction.
        case XE2_REG_CX0_M2P_MSGBUS_CTL(0):
        case XE2_REG_CX0_M2P_MSGBUS_CTL(1): {
            size_t   lane      = (offset - XE2_REG_CX0_M2P_MSGBUS_CTL(0)) / 4;
            uint32_t cmd       = read_uint32_le(data);
            xe2->cx0[lane].m2p = cmd;
            if ((cmd & XE2_REG_CX0_M2P_TRANSACTION_PENDING_MASK) || (cmd & XE2_REG_CX0_M2P_TRANSACTION_RESET_MASK)) {
                xe2_cx0_msgbus_transaction(&xe2->cx0[lane], cmd);
            }
            break;
        }
        case XE2_REG_CX0_P2M_MSGBUS_STATUS(0):
        case XE2_REG_CX0_P2M_MSGBUS_STATUS(1): {
            size_t   lane = (offset - XE2_REG_CX0_P2M_MSGBUS_STATUS(0)) / 4;
            uint32_t cmd  = read_uint32_le(data);
            // Write-1-to-clear RESPONSE_READY and ERROR_SET.
            xe2->cx0[lane].p2m &= ~(cmd & (XE2_REG_CX0_P2M_RESPONSE_READY_MASK | XE2_REG_CX0_P2M_ERROR_SET_MASK));
            break;
        }
        case XE2_REG_XELPDP_PORT_CLOCK_CTL: {
            uint32_t cmd = read_uint32_le(data);
            // Mirror each set PLL/refclk request bit into its ack bit (and clear
            // the ack when the request drops) so the clock-enable polls succeed.
            for (size_t lane = 0; lane < XE2_CX0_LANE_TOTAL; lane++) {
                if (cmd & XE2_REG_XELPDP_PORT_CLOCK_CTL_PLL_REQUEST_MASK(lane)) {
                    cmd |= XE2_REG_XELPDP_PORT_CLOCK_CTL_PLL_ACK_MASK(lane);
                } else {
                    cmd &= ~XE2_REG_XELPDP_PORT_CLOCK_CTL_PLL_ACK_MASK(lane);
                }
                if (cmd & XE2_REG_XELPDP_PORT_CLOCK_CTL_REFCLK_REQUEST_MASK(lane)) {
                    cmd |= XE2_REG_XELPDP_PORT_CLOCK_CTL_REFCLK_ACK_MASK(lane);
                } else {
                    cmd &= ~XE2_REG_XELPDP_PORT_CLOCK_CTL_REFCLK_ACK_MASK(lane);
                }
            }
            xe2->port_clock_ctl = cmd;
            break;
        }
        case XE2_REG_XELPDP_PORT_BUF_CTL1:
            xe2->port_buf_ctl1 = read_uint32_le(data);
            break;
        case XE2_REG_XELPDP_PORT_BUF_CTL2:
            xe2->port_buf_ctl2 = read_uint32_le(data);
            break;
        case XE2_REG_TRANS_DDI_FUNC_CTL_A:
            xe2->trans_ddi_func_ctl = read_uint32_le(data);
            break;
        case XE2_REG_TRANSCONF_A:
            xe2->transconf = read_uint32_le(data);
            break;

        case XE2_REG_BXT_DE_PLL_ENABLE: {
            uint32_t cmd    = read_uint32_le(data);
            xe2->pll_enable = !!xe2_reg_field_get(XE2_REG_BXT_DE_PLL_ENABLE_MASK, cmd);
            break;
        }

        case XE2_REG_PP_STATUS:
            xe2->pp.status = read_uint32_le(data);
            break;
        case XE2_REG_PP_CONTROL:
            xe2->pp.control = read_uint32_le(data);
            if (xe2->pp.control & XE2_REG_PP_CONTROL_POWER_ON_MASK) {
                xe2->pp.status = XE2_REG_PP_ON_MASK | XE2_REG_PP_READY_MASK;
            } else {
                xe2->pp.status &= ~(XE2_REG_PP_ON_MASK | XE2_REG_PP_READY_MASK);
            }
            break;
        case XE2_REG_PP_ON_DELAYS:
            xe2->pp.on_delays = read_uint32_le(data);
            break;
        case XE2_REG_PP_OFF_DELAYS:
            xe2->pp.off_delays = read_uint32_le(data);
            break;

        case XE2_REG_PCH_PP_STATUS:
            xe2->pp_pch.status = read_uint32_le(data);
            break;
        case XE2_REG_PCH_PP_CONTROL:
            xe2->pp_pch.control = read_uint32_le(data);
            break;
        case XE2_REG_PCH_PP_ON_DELAYS:
            xe2->pp_pch.on_delays = read_uint32_le(data);
            break;
        case XE2_REG_PCH_PP_OFF_DELAYS:
            xe2->pp_pch.off_delays = read_uint32_le(data);
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
        case XE2_REG_DC_STATE_EN:
            xe2->dc_state = read_uint32_le(data);
            break;

        case XE2_REG_DMC_SSP_BASE:
            xe2->firmware.dmc_base = read_uint32_le(data);
            break;

        case XE2_REG_GUC_WOPCM_SIZE: {
            uint32_t cmd    = read_uint32_le(data);
            xe2->wopcm_size = xe2_reg_field_get(XE2_REG_GUC_WOPCM_SIZE_MASK, cmd);
            atomic_store_uint32_relax(&xe2->wopcm_locked, 1);
            break;
        }
        case XE2_REG_GUC_WOPCM_OFFSET: {
            uint32_t cmd      = read_uint32_le(data);
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
            uint32_t    cmd  = read_uint32_le(data);
            rvvm_addr_t dma  = cmd & ~xe2_reg_field_prep(xe2_reg_genmask(20, 16), 0x1F);
            xe2->dma_0      |= dma << 32;
            break;
        }

        case XE2_REG_GUC_TLB_INV_DESC0:
            rvvm_info("GuC requested to invalidate TLB[0]");
            break;
        case XE2_REG_GUC_TLB_INV_DESC1:
            rvvm_info("GuC requested to invalidate TLB[1]");
            break;

        case XE2_REG_GUC_DMA_ADDR_1_LO:
            xe2->dma_1 = (rvvm_addr_t)read_uint32_le(data);
            break;
        case XE2_REG_GUC_DMA_ADDR_1_HI: {
            uint32_t    cmd  = read_uint32_le(data);
            rvvm_addr_t dma  = cmd & ~xe2_reg_field_prep(xe2_reg_genmask(20, 16), 0x1F);
            xe2->dma_1      |= dma << 32;
            break;
        }
        case XE2_REG_GUC_DMA_COPY_SIZE:
            xe2->dma_copy_size = read_uint32_le(data);
            break;

        case XE2_REG_GUC_HOST_INTERRUPT:
            xe2_guc_host_interrupt(xe2);
            break;

        case XE2_REG_HW_ENGINE_RING_START(XE2_HW_ENGINE_RENDER_RING_BASE):
            rvvm_info("Write ring start: HW engine renderer: 0x%x", read_uint32_le(data));
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

        default:
            break;
    }

    if (offset >= XE2_DMC_FW_MAIN_OFFSET && offset + size <= XE2_DMC_FW_MAIN_OFFSET + sizeof(xe2->firmware.main)) {
        size_t fw_off = offset - XE2_DMC_FW_MAIN_OFFSET;
        memcpy(xe2->firmware.main + fw_off, data, size);
        if (fw_off + size > xe2->firmware.main_loaded) {
            xe2->firmware.main_loaded = fw_off + size;
        }
    }

    // Shadow pipe and plane registers for modeset verify readback.
    if (offset >= 0x60000 && offset + size <= 0x62000) {
        xe2->display.pipe_regs_shadow[(offset - 0x60000) / 4] = read_uint32_le(data);
    }
    if (offset >= 0x70000 && offset + size <= 0x78000) {
        xe2->display.plane_regs_shadow[(offset - 0x70000) / 4] = read_uint32_le(data);
    }

    // Latch DMC loader register writes so they read back during verification.
    uint8_t* dmc_shadow = xe2_dmc_shadow(xe2, offset);
    if (dmc_shadow != NULL && size <= 4) {
        memcpy(dmc_shadow, data, size);
    }

    spin_unlock(&xe2->lock);
}

static rvvm_reg_type_t xe2_type = {
    .name     = "xe2",
    .read     = xe2_mmio_read,
    .write    = xe2_mmio_write,
    .poll     = xe2_update,
    .cleanup  = xe2_remove,
    .min_size = 4,
    .max_size = 4,
};

static rvvm_reg_type_t xe2_type_vram = {
    .name    = "xe2",
    .cleanup = xe2_remove_vram,
};

RVVM_PUBLIC rvvm_pci_func_t* rvvm_gpu_xe2_init(rvvm_machine_t* machine, rvvm_fbdev_t* fbdev, rvvm_pci_addr_t addr)
{
    xe2_dev_t* xe2           = safe_new_obj(xe2_dev_t);
    xe2->aux[0].edid_written = 0;
    xe2->steer_semaphore     = 1; // Begin with unlocked state.
    xe2->fbdev               = fbdev;
    if (fbdev) {
        rvvm_fbdev_inc_ref(fbdev);
    }
    xe2->vram           = vma_alloc(NULL, XE2_VRAM_SIZE, VMA_RDWR);
    xe2->ggtt_pte       = vma_alloc(NULL, XE2_GGTT_PAGES * sizeof(uint64_t), VMA_RDWR);
    xe2->ggtt_lo_addrs  = vma_alloc(NULL, XE2_GGTT_PAGES * sizeof(uint32_t), VMA_RDWR);
    xe2->ggtt_pte_valid = vma_alloc(NULL, XE2_GGTT_PAGES * sizeof(bool), VMA_RDWR);
    xe2->vulkan_ctx     = gpu_vulkan_create();

    rvvm_reg_desc_t xe2_mmio_desc = {
        .size = 0x1000000,
        .data = xe2,
        .type = &xe2_type,
    };
    rvvm_reg_desc_t xe2_vram_desc = {
        .size = XE2_VRAM_SIZE,
        .type = &xe2_type_vram,
        .mmap = xe2->vram,
    };

    rvvm_pci_func_desc_t xe2_desc = {
        .vendor_id  = XE2_VENDOR_ID_INTEL,
        .device_id  = XE2_DEVICE_ID_ARC_B570_GRAPHICS,
        .class_code = XE2_CLASS_CODE,
        .prog_iface = 0,
        .irq_pin    = RVVM_PCI_PIN_INTA,
        .bar[0]     = &xe2_mmio_desc,
        .bar[2]     = &xe2_vram_desc,
    };

    rvvm_pci_func_t* func = rvvm_pci_func_init(machine, &xe2_desc, addr);
    if (func) {
        // Successfully plugged in
        xe2->pci_func = func;
    }
    return func;
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
