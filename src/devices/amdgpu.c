/*
amdgpu.c - AMD graphic cards
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "amdgpu.h"
#include "amdgpu_bios.h"
#include "compiler.h"
#include "rvvm/rvvm_base.h"
#include "utils.h"
#include "mem_ops.h"
#include <stdint.h>

/* https://rocm.docs.amd.com/en/docs-6.4.3/how-to/Bar-Memory.html#bar-configuration-for-amd-gpus
 *
 * BAR 0: GPU memory mapping (prefetchable)
 * BAR 1: GPU memory mapping (prefetchable)
 * BAR 2: Doorbell (prefetchable)
 * BAR 3: Doorbell (prefetchable)
 * BAR 4: Optional (?)
 * BAR 5: 32-bit MMIO
 *
 * IP blocks defined (drm/amdgpu_drm.h):
 * #define AMDGPU_HW_IP_GFX          0
 * #define AMDGPU_HW_IP_COMPUTE      1
 * #define AMDGPU_HW_IP_DMA          2
 * #define AMDGPU_HW_IP_UVD          3
 * #define AMDGPU_HW_IP_VCE          4
 * #define AMDGPU_HW_IP_UVD_ENC      5
 * #define AMDGPU_HW_IP_VCN_DEC      6
 * #define AMDGPU_HW_IP_VCN_ENC      7
 * #define AMDGPU_HW_IP_VCN_JPEG     8
 * #define AMDGPU_HW_IP_VPE          9
 * #define AMDGPU_HW_IP_NUM          10
 *
 * IP blocks:
 * [   72.444509] amdgpu 0000:00:01.0: amdgpu: detected ip block number 0 <vi_common>
 * [   72.444630] amdgpu 0000:00:01.0: amdgpu: detected ip block number 1 <gmc_v8_0>
 * [   72.444681] amdgpu 0000:00:01.0: amdgpu: detected ip block number 2 <tonga_ih>
 * [   72.444738] amdgpu 0000:00:01.0: amdgpu: detected ip block number 3 <gfx_v8_0>
 * [   72.444774] amdgpu 0000:00:01.0: amdgpu: detected ip block number 4 <sdma_v3_0>
 * [   72.444809] amdgpu 0000:00:01.0: amdgpu: detected ip block number 5 <powerplay>
 * [   72.444866] amdgpu 0000:00:01.0: amdgpu: detected ip block number 6 <dm>
 * [   72.444904] amdgpu 0000:00:01.0: amdgpu: detected ip block number 7 <uvd_v6_0>
 * [   72.444941] amdgpu 0000:00:01.0: amdgpu: detected ip block number 8 <vce_v3_0>
 *
 * amdgpu/vi.c:
 *  int vi_set_ip_blocks(struct amdgpu_device *adev)
 * 	{
 * 	    case CHIP_TONGA:
 *  		amdgpu_device_ip_block_add(adev, &vi_common_ip_block);
 *  		amdgpu_device_ip_block_add(adev, &gmc_v8_0_ip_block);
 *  		amdgpu_device_ip_block_add(adev, &tonga_ih_ip_block);
 *  		amdgpu_device_ip_block_add(adev, &gfx_v8_0_ip_block);
 *  		amdgpu_device_ip_block_add(adev, &sdma_v3_0_ip_block);
 *  		amdgpu_device_ip_block_add(adev, &pp_smu_ip_block);
 *  		if (adev->enable_virtual_display)
 *  			amdgpu_device_ip_block_add(adev, &amdgpu_vkms_ip_block);
 *  #if defined(CONFIG_DRM_AMD_DC)
 *  		else if (amdgpu_device_has_dc_support(adev))
 *  			amdgpu_device_ip_block_add(adev, &dm_ip_block);
 *  #endif
 *  		else
 *  			amdgpu_device_ip_block_add(adev, &dce_v10_0_ip_block);
 *  		if (!amdgpu_sriov_vf(adev)) {
 *  			amdgpu_device_ip_block_add(adev, &uvd_v5_0_ip_block);
 *  			amdgpu_device_ip_block_add(adev, &vce_v3_0_ip_block);
 *  		}
 *  		break;
 *  }
 *
 * Possibly important functions:
 * // vi - Volcanic Islands
 * static int vi_common_early_init(struct amdgpu_ip_block *ip_block);
 *
 *
 *********************************************************************
 * Initialization:
 * [root@archlinux ~]# [   74.494168] amdgpu: DSDT table not found for OEM information
 * RVVM: AMDGPU region 5 read: offset=3f0c
 * RVVM: AMDGPU region 5 read: offset=1c
 * RVVM: AMDGPU region 5 write: offset=6b0, data=c0600010
 * RVVM: AMDGPU region 5 write: offset=6b4, data=0
 * RVVM: AMDGPU region 5 write: offset=6b0, data=c0600014
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 read: offset=5420
 * RVVM: AMDGPU region 5 write: offset=6b0, data=c0600000
 * RVVM: AMDGPU region 5 read: offset=6b4
 * RVVM: AMDGPU region 5 write: offset=5420, data=0
 * RVVM: AMDGPU region 5 write: offset=6b0, data=c0600000
 * RVVM: AMDGPU region 5 write: offset=6b4, data=c0600002
 * RVVM: AMDGPU region 5 write: offset=5420, data=0
 * RVVM: AMDGPU region 5 write: offset=6b0, data=c0600000
 * RVVM: AMDGPU region 5 write: offset=6b4, data=c0600000
 * [   74.611984] amdgpu 0000:00:02.0: amdgpu: Unable to locate a BIOS ROM
 * [   74.613713] amdgpu 0000:00:02.0: amdgpu: Fatal error during GPU init
 * [   74.615324] amdgpu 0000:00:02.0: probe with driver amdgpu failed with error -22
 *
 ***********************************************************************
 *
 * Since BIOS stage passed, kernel now reports:
 *
 * [   77.787345] [drm] amdgpu kernel modesetting enabled.
 * [   77.796156] amdgpu: DSDT table not found for OEM information
 * [   77.802050] amdgpu: IO link not available for non x86 platforms
 * [   77.802120] amdgpu: Virtual CRAT table created for CPU
 * [   77.802774] amdgpu: Topology: Add CPU node
 * [   77.808088] amdgpu 0000:00:02.0: amdgpu: initializing kernel modesetting (POLARIS10 0x1002:0x67DF 0x10DC:0x5100 0x00).
 * [   77.808637] amdgpu 0000:00:02.0: amdgpu: register mmio base: 0x40400000
 * [   77.808685] amdgpu 0000:00:02.0: amdgpu: register mmio size: 262144
 * [   77.845352] amdgpu 0000:00:02.0: amdgpu: detected ip block number 0 <vi_common>
 * [   77.845456] amdgpu 0000:00:02.0: amdgpu: detected ip block number 1 <gmc_v8_0>
 * [   77.845508] amdgpu 0000:00:02.0: amdgpu: detected ip block number 2 <tonga_ih>
 * [   77.845546] amdgpu 0000:00:02.0: amdgpu: detected ip block number 3 <gfx_v8_0>
 * [   77.845582] amdgpu 0000:00:02.0: amdgpu: detected ip block number 4 <sdma_v3_0>
 * [   77.845617] amdgpu 0000:00:02.0: amdgpu: detected ip block number 5 <powerplay>
 * [   77.845672] amdgpu 0000:00:02.0: amdgpu: detected ip block number 6 <dm>
 * [   77.845713] amdgpu 0000:00:02.0: amdgpu: detected ip block number 7 <uvd_v6_0>
 * [   77.845748] amdgpu 0000:00:02.0: amdgpu: detected ip block number 8 <vce_v3_0>
 * [   78.324900] amdgpu 0000:00:02.0: amdgpu: Fetched VBIOS from ROM BAR
 * [   78.325136] amdgpu: ATOM BIOS: 113-D0090101-100
 * [   78.358823] amdgpu 0000:00:02.0: amdgpu: Trusted Memory Zone (TMZ) feature not supported
 * [   78.359180] amdgpu 0000:00:02.0: amdgpu: PCI CONFIG reset
 * [   78.359460] amdgpu 0000:00:02.0: amdgpu: GPU posting now...
 * [   98.500094] [drm:atom_op_jump [amdgpu]] *ERROR* atombios stuck in loop for more than 20secs aborting
 * [   98.549730] [drm:amdgpu_atom_execute_table_locked [amdgpu]] *ERROR* atombios stuck executing AE7E (len 403, WS 20, PS 0) @ 0xAF97
 * [   98.600839] [drm:amdgpu_atom_execute_table_locked [amdgpu]] *ERROR* atombios stuck executing AADC (len 133, WS 0, PS 8) @ 0xAB36
 * [   98.652268] amdgpu 0000:00:02.0: amdgpu: gpu post error!
 * [   98.653072] amdgpu 0000:00:02.0: amdgpu: Fatal error during GPU init
 * [   98.653885] amdgpu 0000:00:02.0: amdgpu: amdgpu: finishing device.
 * [   98.654333] amdgpu 0000:00:02.0: probe with driver amdgpu failed with error -22
 *
 * Some ATOMBIOS scripts running.
 *
 ************************************************************************
 *
 * Does PCI extended capability supported?
 * #define PCI_EXT_CAP_ID_ERR	0x01	/ * Advanced Error Reporting * /
 * #define PCI_EXT_CAP_ID_VC	0x02	/ * Virtual Channel Capability * /
 * #define PCI_EXT_CAP_ID_DSN	0x03	/ * Device Serial Number * /
 * #define PCI_EXT_CAP_ID_PWR	0x04	/ * Power Budgeting * /
 * #define PCI_EXT_CAP_ID_RCLD	0x05	/ * Root Complex Link Declaration * /
 * #define PCI_EXT_CAP_ID_RCILC	0x06	/ * Root Complex Internal Link Control * /
 * #define PCI_EXT_CAP_ID_RCEC	0x07	/ * Root Complex Event Collector * /
 * #define PCI_EXT_CAP_ID_MFVC	0x08	/ * Multi-Function VC Capability * /
 * #define PCI_EXT_CAP_ID_VC9	0x09	/ * same as _VC * /
 * #define PCI_EXT_CAP_ID_RCRB	0x0A	/ * Root Complex RB? * /
 * #define PCI_EXT_CAP_ID_VNDR	0x0B	/ * Vendor-Specific * /
 *
 *************************************************************************
 *
 * Next stage: Initialize VRAM.
 *
 * 	r = amdgpu_vram_mgr_init(adev);
 *	if (r) {
 *		dev_err(adev->dev, "Failed initializing VRAM heap.\n");
 *		return r;
 *	}
 *
 *	amdgpu_vram_mgr_init() uses data collected before to initialize VRAM,
 *	so he successfully collected 0 bytes.
 *
 **************************************************************************
 *
 * Вывод: выбросить это говно нахуй и сделать Intel XE2.
 */

#define AMDGPU_VENDOR_ID_AMD     0x8086
#define AMDGPU_DEVICE_POLARIS_10 0x67DF
#define AMDGPU_CLASS_CODE        0x0300

typedef struct {
    pci_func_t *pci_func;
} amdgpu_dev_t;

static void amdgpu_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t amdgpu_type = {
    .name = "amdgpu",
    .remove = amdgpu_remove,
};

static bool amdgpu_mmio_read_region0(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // rvvm_info("AMDGPU region 0 read: offset=%zx", offset);
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_write_region0(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // rvvm_info("AMDGPU region 0 write: offset=%zx, data=%x", offset, read_uint32_le(data));
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_read_region2(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // rvvm_info("AMDGPU region 2 read: offset=%zx", offset);
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_write_region2(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // rvvm_info("AMDGPU region 2 write: offset=%zx, data=%x", offset, read_uint32_le(data));
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_read_region4(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // rvvm_info("AMDGPU region 4 read: offset=%zx", offset);
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_write_region4(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("AMDGPU region 4 write: offset=%zx, data=%x", offset, read_uint32_le(data));
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_read_region5(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // rvvm_info("AMDGPU region 5 read: offset=%zx", offset);

    /* We need to evaluate some ATOMBIOS bytecode. */
    switch (offset) {
    case 0x809:
        rvvm_info("Possible VRAM location");
        break;
    default:
        break;
    }

    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_write_region5(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    // rvvm_info("AMDGPU region 5 write: offset=%zx, data=%x", offset, read_uint32_le(data));
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_read_expansion_rom(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    UNUSED(dev);
    /* Note that in practice BIOS always 4-byte aligned so we probably will not encounter
     * alignment or buffer-overflow issues. */
    memcpy(data, amdgpu_bios_rx480_8192_160603 + offset, size);

    return true;
}

static bool amdgpu_mmio_write_expansion_rom(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

PUBLIC pci_dev_t *amdgpu_init(pci_bus_t *pci_bus)
{
    amdgpu_dev_t *amdgpu_dev = safe_new_obj(amdgpu_dev_t);
    rvvm_info("%s", __FUNCTION__);

    pci_func_desc_t amdgpu_desc = {
        .vendor_id  = AMDGPU_VENDOR_ID_AMD,
        .device_id  = AMDGPU_DEVICE_POLARIS_10,
        .class_code = AMDGPU_CLASS_CODE,
        .prog_if    = 0x00,
        .irq_pin    = PCI_IRQ_PIN_INTA,
        /* Used to load the BIOS. */
        .expansion_rom = {
            .size        = 0x200000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read        = amdgpu_mmio_read_expansion_rom,
            .write       = amdgpu_mmio_write_expansion_rom,
            .data        = amdgpu_dev,
            .type        = &amdgpu_type
        },
        /* VRAM mapping (maybe unused). */
        .bar[0]     = {
            .size        = 0x200000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read        = amdgpu_mmio_read_region0,
            .write       = amdgpu_mmio_write_region0,
            .data        = amdgpu_dev,
            .type        = &amdgpu_type
        },
        /* Doorbell. */
        .bar[2]     = {
            .size        = 0x4000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read        = amdgpu_mmio_read_region2,
            .write       = amdgpu_mmio_write_region2,
            .data        = amdgpu_dev,
            .type        = &amdgpu_type
        },
        /* Optional BAR (maybe unused). */
        .bar[4]     = {
            .size        = 0x4000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read        = amdgpu_mmio_read_region4,
            .write       = amdgpu_mmio_write_region4,
            .data        = amdgpu_dev,
            .type        = &amdgpu_type
        },
        /* MMIO. */
        .bar[5]     = {
            .size        = 0x40000,
            .min_op_size = 4,
            .max_op_size = 4,
            .read        = amdgpu_mmio_read_region5,
            .write       = amdgpu_mmio_write_region5,
            .data        = amdgpu_dev,
            .type        = &amdgpu_type
        }
    };

    pci_dev_t *pci_dev = pci_attach_func(pci_bus, &amdgpu_desc);
    if (pci_dev)
        amdgpu_dev->pci_func = pci_get_device_func(pci_dev, 0);

    return pci_dev;
}

PUBLIC pci_dev_t *amdgpu_init_auto(rvvm_machine_t *machine)
{
    return amdgpu_init(rvvm_get_pci_bus(machine));
}
