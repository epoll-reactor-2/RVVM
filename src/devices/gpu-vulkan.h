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

gpu_vulkan_ctx_t *gpu_vulkan_create(void);

void gpu_vulkan_destroy(gpu_vulkan_ctx_t *ctx);

bool gpu_vulkan_render_frame(gpu_vulkan_ctx_t *ctx, uint32_t width, uint32_t height,
                              uint8_t *dst, size_t dst_size, uint32_t stride, rvvm_rgb_t format,
                              uint32_t *out_width, uint32_t *out_height,
                              uint32_t *out_stride, rvvm_rgb_t *out_format);

#endif /* RVVM_GPU_VULKAN_H */
