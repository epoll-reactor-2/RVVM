/*
gpu-xe2-shader.h - Intel XE2 kernel -> SPIR-V shader builder
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_GPU_XE2_SHADER_H
#define RVVM_GPU_XE2_SHADER_H

#include "gpu-vulkan-spirv.h"
#include "gpu-xe2-3dstate.h"
#include <util/utils.h>

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

#endif /* RVVM_GPU_XE2_SHADER_H */
