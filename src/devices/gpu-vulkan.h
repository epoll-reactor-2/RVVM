/*
gpu-vulkan.h - Vulkan GPU backend
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_GPU_VULKAN_H
#define RVVM_GPU_VULKAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gpu_vulkan_ctx_t gpu_vulkan_ctx_t;

gpu_vulkan_ctx_t *gpu_vulkan_create(void);

void gpu_vulkan_destroy(gpu_vulkan_ctx_t *ctx);

bool gpu_vulkan_render_frame(gpu_vulkan_ctx_t *ctx, uint32_t w, uint32_t h, const uint8_t **out_pixels, size_t *out_row_pitch);

#endif /* RVVM_GPU_VULKAN_H */
