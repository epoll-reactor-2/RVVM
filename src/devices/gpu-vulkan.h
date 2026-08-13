/*
gpu-vulkan.h - Vulkan GPU backend
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_GPU_VULKAN_H
#define RVVM_GPU_VULKAN_H

#include "rvvm/rvvm_base.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gpu_vulkan_ctx_t gpu_vulkan_ctx_t;

gpu_vulkan_ctx_t* gpu_vulkan_create(void);

void gpu_vulkan_destroy(gpu_vulkan_ctx_t* ctx);

// -----------------------------------------------------------
// Guest-driven draws
// -----------------------------------------------------------

// Programmable stages a draw may carry, in the order the graphics
// pipeline runs them.
typedef enum {
    GPU_VULKAN_STAGE_VERTEX = 0,
    GPU_VULKAN_STAGE_TESS_CTRL,
    GPU_VULKAN_STAGE_TESS_EVAL,
    GPU_VULKAN_STAGE_GEOMETRY,
    GPU_VULKAN_STAGE_FRAGMENT,
    GPU_VULKAN_STAGE_COUNT
} gpu_vulkan_stage_t;

// Values match VkPrimitiveTopology.
typedef enum {
    GPU_VULKAN_TOPOLOGY_POINT_LIST     = 0,
    GPU_VULKAN_TOPOLOGY_LINE_LIST      = 1,
    GPU_VULKAN_TOPOLOGY_LINE_STRIP     = 2,
    GPU_VULKAN_TOPOLOGY_TRIANGLE_LIST  = 3,
    GPU_VULKAN_TOPOLOGY_TRIANGLE_STRIP = 4,
    GPU_VULKAN_TOPOLOGY_TRIANGLE_FAN   = 5,
} gpu_vulkan_topology_t;

// Every stage gets its constants bound as a uniform block of this size,
// at descriptor set GPU_VULKAN_CONST_SET, binding = the stage index. A
// shader handed to gpu_vulkan_submit_draw() must declare its block to
// match; the emulated device does that in gpu-xe2-shader.h.
#define GPU_VULKAN_CONST_SET   0
#define GPU_VULKAN_CONST_BYTES 1024

typedef struct {
    // SPIR-V module for this stage. NULL leaves the stage disabled.
    // Copied by gpu_vulkan_submit_draw(), so the caller stays free to
    // recompile or free it right after.
    const uint32_t* spirv;
    uint32_t        spirv_nwords;

    // Contents of the stage's uniform block. Anything past const_bytes
    // reads back as zero.
    const void* constants;
    uint32_t    const_bytes;
} gpu_vulkan_stage_desc_t;

typedef struct {
    gpu_vulkan_stage_desc_t stage[GPU_VULKAN_STAGE_COUNT];

    uint32_t topology; // gpu_vulkan_topology_t
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} gpu_vulkan_draw_t;

/*
 * Hand a draw to the renderer.
 *
 * The draw is copied and kept as the scene the backend renders from that
 * point on; the next render tick picks it up. A later submission
 * replaces it, so a guest issuing several draws per frame currently gets
 * the last one - there is no command list yet.
 *
 * Returns false when the draw carries no vertex shader - a graphics
 * pipeline cannot be built without one - in which case the backend keeps
 * rendering whatever it had.
 */
bool gpu_vulkan_submit_draw(gpu_vulkan_ctx_t* ctx, const gpu_vulkan_draw_t* draw);

// -----------------------------------------------------------
// Scanout
// -----------------------------------------------------------

bool gpu_vulkan_render_frame(gpu_vulkan_ctx_t* ctx, uint32_t width, uint32_t height, uint8_t* dst, size_t dst_size,
                             uint32_t stride, rvvm_rgb_t format, uint32_t* out_width, uint32_t* out_height,
                             uint32_t* out_stride, rvvm_rgb_t* out_format);

#endif /* RVVM_GPU_VULKAN_H */
