/*
amdgpu.c - AMD graphic cards
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "amdgpu.h"
#include "utils.h"
#include "mem_ops.h"

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
 */

#define AMDGPU_VENDOR_ID_AMD     0x1002
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
    rvvm_info("AMDGPU region 0 read: offset=%zx", offset);
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_write_region0(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("AMDGPU region 0 write: offset=%zx, data=%x", offset, read_uint32_le(data));
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_read_region2(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("AMDGPU region 2 read: offset=%zx", offset);
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_write_region2(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("AMDGPU region 2 write: offset=%zx, data=%x", offset, read_uint32_le(data));
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_read_region4(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("AMDGPU region 4 read: offset=%zx", offset);
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
    rvvm_info("AMDGPU region 5 read: offset=%zx", offset);
    UNUSED(dev);
    UNUSED(data);
    UNUSED(offset);
    UNUSED(size);
    return true;
}

static bool amdgpu_mmio_write_region5(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    rvvm_info("AMDGPU region 5 write: offset=%zx, data=%x", offset, read_uint32_le(data));
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
