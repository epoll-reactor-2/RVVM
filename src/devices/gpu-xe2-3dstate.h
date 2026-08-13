/*
gpu-xe2-3dstate.h - Intel XE2 3D pipeline state
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_GPU_XE2_3DSTATE_H
#define RVVM_GPU_XE2_3DSTATE_H

#include "rvvm/rvvm_base.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

#define XE2_SHADER_MAX_BINDINGS 16
#define XE2_SHADER_MAX_GRF      128

#define XE2_MEM_SMEM            0 // System memory (accessed via DMA)
#define XE2_MEM_LMEM            1 // Local memory (accessed via VRAM)

typedef struct {
    uint64_t addr;
    uint8_t  type;
} xe2_dma_addr_t;

// -----------------------------------------------------------
// Push constants (3DSTATE_CONSTANT_XS)
// -----------------------------------------------------------

// Xe2 dispatches a thread with the constant buffer contents already
// resident in its GRFs, right after the fixed payload registers. A Xe2
// GRF is 64 bytes wide; the constant gather works in 32-byte chunks,
// which is also the unit the Read Length fields are expressed in.
#define XE2_GRF_BYTES         64
#define XE2_GRF_DWORDS        (XE2_GRF_BYTES / 4)
#define XE2_CONST_CHUNK_BYTES 32

// 3DSTATE_CONSTANT_BODY exposes four buffers per stage. The hardware
// concatenates them, in index order, into the pushed GRF payload; Mesa
// uses buffer 0 for the inline uniforms and buffers 1..3 for pushed UBO
// ranges.
#define XE2_CONST_BUFFERS     4

// Upper bound on the gathered payload we mirror. Also the size of the
// uniform block the cross-compiled shader declares and of the buffer
// the Vulkan backend uploads, so all three agree by construction.
#define XE2_CONST_MAX_VEC4    64
#define XE2_CONST_MAX_DWORDS  (XE2_CONST_MAX_VEC4 * 4)
#define XE2_CONST_MAX_BYTES   (XE2_CONST_MAX_DWORDS * 4)

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
        consts->buffer[i].va = ((((uint64_t)hi) << 32) | lo) & ~0x1FULL;
    }
}

// Total gathered payload size for the decoded buffers, clamped to what
// we mirror. Buffers with no address contribute nothing.
static inline uint32_t xe2_const_payload_size(const xe2_push_const_t* consts)
{
    uint32_t nbytes = 0;
    for (uint32_t i = 0; i < XE2_CONST_BUFFERS; ++i) {
        if (consts->buffer[i].va) {
            nbytes += consts->buffer[i].read_length * XE2_CONST_CHUNK_BYTES;
        }
    }
    return (nbytes > XE2_CONST_MAX_BYTES) ? XE2_CONST_MAX_BYTES : nbytes;
}

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

// What a 3DPRIMITIVE asks for, on top of the state around it.
typedef struct {
    uint32_t topology;
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} xe2_draw_params_t;

typedef struct {
    xe2_shader_stage_t shader[XE2_SHADER_STAGE_COUNT];
    xe2_push_const_t   consts[XE2_SHADER_STAGE_COUNT];

    // 3DSTATE_BINDING_TABLE_POINTERS_XS, relative to the surface state
    // base. Not populated yet - surface binding is unimplemented.
    uint32_t binding_table_offset[XE2_SHADER_STAGE_COUNT];

    xe2_vertex_input_t vertex_input;
    xe2_ff_state_t     ff;
    bool               ff_dirty;

    // The draw last handed to the renderer. An identical draw against
    // unchanged state does not need to be handed over again.
    xe2_draw_params_t last_draw;
    bool              last_draw_valid;
} xe2_3dstate_t;

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

// Dword offset into the gathered payload that a (GRF, subregister) pair
// addresses. Only meaningful for registers at or above the stage's push
// constant base; the caller checks that and the XE2_CONST_MAX_DWORDS
// bound before using the result.
static inline uint32_t xe2_const_dword_index(uint32_t grf, uint32_t subreg, uint32_t push_grf_base)
{
    return (grf - push_grf_base) * XE2_GRF_DWORDS + subreg;
}

#endif /* RVVM_GPU_XE2_3DSTATE_H */
