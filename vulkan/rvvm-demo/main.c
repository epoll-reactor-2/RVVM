#include "devices/gpu-vulkan.h"
#include "gpu-vulkan-spirv.h"
#include "gpu-vulkan.h"
#include "utils.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_render.h>
#include <SDL2/SDL_video.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RVVM_RGB_RGB565
#define RVVM_RGB_RGB565      0x02
#define RVVM_RGB_XRGB8888    0x04
#define RVVM_RGB_XBGR8888    0x05
#define RVVM_RGB_XRGB2101010 0x06
#endif

// 1: load precompiled vert.spv/frag.spv instead of building the shaders in
// this file. Such a fragment shader must declare the same constant block as
// spirv_compile_triangle_fragment() below (GLSL given in its comment).
#ifndef DEMO_SHADERS_FROM_DISK
#define DEMO_SHADERS_FROM_DISK 0
#endif

static void spirv_dump_disk(const char* path, const uint32_t* spirv, uint32_t n)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        rvvm_fatal("fopen(): %s", strerror(errno));
    }
    fwrite(spirv, 4, n, f);
    fclose(f);
}

#if DEMO_SHADERS_FROM_DISK
static void spirv_read_disk(const char* path, uint32_t** out, uint32_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        rvvm_fatal("fopen(): %s", strerror(errno));
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = safe_calloc(1, size);
    fread(*out, 1, size, f);
    fclose(f);
    *n = size / 4;

    rvvm_info("Read %s: %u bytes", path, *n);
}
#endif

// #version 450
//
// vec2 positions[3] = vec2[](
//     vec2( 0.0, -0.5),
//     vec2( 0.5,  0.5),
//     vec2(-0.5,  0.5)
// );
//
// vec3 colors[3] = vec3[](
//     vec3(1.0, 0.0, 0.0),
//     vec3(0.0, 1.0, 0.0),
//     vec3(0.0, 0.0, 1.0)
// );
//
// layout(location = 0) out vec3 fragColor;
//
// void main() {
//     gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
//     fragColor = colors[gl_VertexIndex];
// }
static int spirv_compile_triangle_vertex(uint32_t** out_vs, uint32_t* out_vs_n)
{
    spirv_module_t vs = {0};
    spirv_module_init(&vs);
    spirv_module_begin(&vs);

    uint32_t void_ty = spirv_type_void(&vs);
    uint32_t f32     = spirv_type_float32(&vs);
    uint32_t i32     = spirv_type_int32(&vs);
    uint32_t v3      = spirv_type_vec3_float32(&vs);
    uint32_t v4      = spirv_type_vec4_float32(&vs);
    uint32_t bool_ty = spirv_type_bool(&vs);
    uint32_t fn_ty   = spirv_type_func_void(&vs);
    uint32_t pos_ptr = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_OUTPUT, v4);
    uint32_t pos_var = spirv_global_var(&vs, pos_ptr, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&vs, pos_var, SPIRV_DECORATION_BUILTIN, SPIRV_BUILTIN_POSITION);
    uint32_t col_ptr = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_OUTPUT, v3);
    uint32_t col_var = spirv_global_var(&vs, col_ptr, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&vs, col_var, SPIRV_DECORATION_LOCATION, 0);
    uint32_t vid_ptr = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_INPUT, i32);
    uint32_t vid_var = spirv_global_var(&vs, vid_ptr, SPIRV_STORAGE_CLASS_INPUT);
    spirv_decorate_1(&vs, vid_var, SPIRV_DECORATION_BUILTIN, SPIRV_BUILTIN_VERTEX_INDEX);
    uint32_t main = spirv_func_begin(&vs, void_ty, fn_ty);
    uint32_t vid  = spirv_op_load(&vs, i32, vid_var);
    uint32_t c0   = spirv_type_const_int32(&vs, 0);
    uint32_t c1   = spirv_type_const_int32(&vs, 1);
    uint32_t eq0  = spirv_cmp(&vs, SPIRV_OP_I_EQUAL, bool_ty, vid, c0);
    uint32_t eq1  = spirv_cmp(&vs, SPIRV_OP_I_EQUAL, bool_ty, vid, c1);
    uint32_t px0  = spirv_type_const_float32(&vs, 0.0f);
    uint32_t py0  = spirv_type_const_float32(&vs, -0.5f);
    uint32_t px1  = spirv_type_const_float32(&vs, 0.5f);
    uint32_t py1  = spirv_type_const_float32(&vs, 0.5f);
    uint32_t px2  = spirv_type_const_float32(&vs, -0.5f);
    uint32_t py2  = spirv_type_const_float32(&vs, 0.5f);
    uint32_t z0   = spirv_type_const_float32(&vs, 0.0f);
    uint32_t w1   = spirv_type_const_float32(&vs, 1.0f);
    uint32_t sx   = spirv_select(&vs, f32, eq0, px0, spirv_select(&vs, f32, eq1, px1, px2));
    uint32_t sy   = spirv_select(&vs, f32, eq0, py0, spirv_select(&vs, f32, eq1, py1, py2));
    uint32_t pos  = spirv_composite_construct4(&vs, v4, sx, sy, z0, w1);
    spirv_op_store(&vs, pos_var, pos);
    uint32_t one  = w1;
    uint32_t zero = z0;
    uint32_t cr   = spirv_select(&vs, f32, eq0, one, spirv_select(&vs, f32, eq1, zero, zero)); /* 1,0,0 */
    uint32_t cg   = spirv_select(&vs, f32, eq0, zero, spirv_select(&vs, f32, eq1, one, zero)); /* 0,1,0 */
    uint32_t cb   = spirv_select(&vs, f32, eq0, zero, spirv_select(&vs, f32, eq1, zero, one)); /* 0,0,1 */
    uint32_t col  = spirv_op_3(&vs, SPIRV_OP_COMPOSITE_CONSTRUCT, v3, cr, cg, cb);
    spirv_op_store(&vs, col_var, col);
    spirv_func_end(&vs);

    uint32_t vs_iface[] = {pos_var, col_var, vid_var};
    spirv_entry_point(&vs, SPIRV_EXECUTION_MODEL_VERTEX, main, "main", vs_iface, 3);

    if (spirv_module_finish(&vs, out_vs, out_vs_n) != 0) {
        spirv_module_free(&vs);
        return -1;
    }

    spirv_dump_disk("/tmp/vs.spv", *out_vs, *out_vs_n);
    spirv_module_free(&vs);
    return 0;
}

// Fragment stage constants, as this driver lays them out. The block is raw
// bytes to the backend; dword 0 is c[0].x in the shader below. Everything
// else in the block stays zero.
typedef struct {
    float hue; // Hue rotation angle, radians.
} triangle_frag_consts_t;

_Static_assert(sizeof(triangle_frag_consts_t) <= GPU_VULKAN_CONST_BYTES, "constants do not fit the block");

// Dot product of a constant 3-vector row with (r, g, b): 3 fmul + 2 fadd.
static uint32_t spirv_row3(spirv_module_t* m, uint32_t f32, uint32_t k0, uint32_t k1, uint32_t k2, uint32_t r,
                           uint32_t g, uint32_t b)
{
    uint32_t t0 = spirv_op_fmul(m, f32, k0, r);
    uint32_t t1 = spirv_op_fmul(m, f32, k1, g);
    uint32_t t2 = spirv_op_fmul(m, f32, k2, b);
    return spirv_op_fadd(m, f32, spirv_op_fadd(m, f32, t0, t1), t2);
}

// #version 450
//
// layout(location = 0)  in vec3 fragColor;
// layout(location = 0) out vec4 outColor;
//
// // Descriptor set GPU_VULKAN_CONST_SET, binding GPU_VULKAN_CONST_BINDING(
// // GPU_VULKAN_STAGE_FRAGMENT) == 4. GPU_VULKAN_CONST_BYTES / 16 == 64.
// layout(set = 0, binding = 4, std140) uniform FragConsts {
//     vec4 c[64];
// };
//
// void main() {
//     float a = c[0].x;         // triangle_frag_consts_t.hue
//     float k = cos(a);
//     float s = sin(a);
//     float d = (1.0 + 2.0 * k) / 3.0;
//     float e = (1.0 - k) / 3.0;
//     float f = s * 0.57735027; // s / sqrt(3)
//     vec3  p = fragColor;
//
//     // Rotation of the color around the gray axis (1, 1, 1).
//     outColor = vec4(d       * p.r + (e - f) * p.g + (e + f) * p.b,
//                     (e + f) * p.r + d       * p.g + (e - f) * p.b,
//                     (e - f) * p.r + (e + f) * p.g + d       * p.b,
//                     1.0);
// }
static int spirv_compile_triangle_fragment(uint32_t** out_fs, uint32_t* out_fs_n)
{
    spirv_module_t fs = {0};
    spirv_module_init(&fs);
    spirv_module_begin(&fs);

    uint32_t void_ty = spirv_type_void(&fs);
    uint32_t fn_ty   = spirv_type_func_void(&fs);
    uint32_t f32     = spirv_type_float32(&fs);
    uint32_t v4      = spirv_type_vec4_float32(&fs);
    uint32_t out_ptr = spirv_type_ptr(&fs, SPIRV_STORAGE_CLASS_OUTPUT, v4);
    uint32_t out_col = spirv_global_var(&fs, out_ptr, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&fs, out_col, SPIRV_DECORATION_LOCATION, 0);
    spirv_name(&fs, out_col, "outColor");
    uint32_t v3     = spirv_type_vec3_float32(&fs);
    uint32_t in_ptr = spirv_type_ptr(&fs, SPIRV_STORAGE_CLASS_INPUT, v3);
    uint32_t in_col = spirv_global_var(&fs, in_ptr, SPIRV_STORAGE_CLASS_INPUT);
    spirv_decorate_1(&fs, in_col, SPIRV_DECORATION_LOCATION, 0);
    spirv_name(&fs, in_col, "fragColor");

    // The stage's constant block: "vec4 c[GPU_VULKAN_CONST_BYTES / 16]" at
    // the set/binding the backend binds it to. Not part of the entry point
    // interface (SPIR-V 1.3 lists only Input/Output variables).
    uint32_t c_elem_ptr = 0;
    uint32_t c_var      = spirv_uniform_vec4_array_block(&fs, GPU_VULKAN_CONST_BYTES / 16, GPU_VULKAN_CONST_SET,
                                                         GPU_VULKAN_CONST_BINDING(GPU_VULKAN_STAGE_FRAGMENT), &c_elem_ptr);
    spirv_name(&fs, c_var, "consts");

    uint32_t main = spirv_func_begin(&fs, void_ty, fn_ty);
    spirv_name(&fs, main, "main");

    // a = c[0].x  (block member 0, array element 0, vector component 0)
    uint32_t zero   = spirv_type_const_uint32(&fs, 0);
    uint32_t idx[3] = {zero, zero, zero};
    uint32_t a_ptr  = spirv_access_chain(&fs, c_elem_ptr, c_var, idx, 3);
    uint32_t a      = spirv_op_load(&fs, f32, a_ptr);

    uint32_t one     = spirv_type_const_float32(&fs, 1.0f);
    uint32_t two     = spirv_type_const_float32(&fs, 2.0f);
    uint32_t third   = spirv_type_const_float32(&fs, 1.0f / 3.0f);
    uint32_t inv_sq3 = spirv_type_const_float32(&fs, 0.57735027f);

    uint32_t k   = spirv_ext_inst1(&fs, f32, SPIRV_GLSL_STD450_COS, a);
    uint32_t s   = spirv_ext_inst1(&fs, f32, SPIRV_GLSL_STD450_SIN, a);
    uint32_t d   = spirv_op_fmul(&fs, f32, spirv_op_fadd(&fs, f32, one, spirv_op_fmul(&fs, f32, two, k)), third);
    uint32_t e   = spirv_op_fmul(&fs, f32, spirv_op_fsub(&fs, f32, one, k), third);
    uint32_t f   = spirv_op_fmul(&fs, f32, s, inv_sq3);
    uint32_t emf = spirv_op_fsub(&fs, f32, e, f);
    uint32_t epf = spirv_op_fadd(&fs, f32, e, f);

    uint32_t rgb  = spirv_op_load(&fs, v3, in_col);
    uint32_t r    = spirv_composite_extract1(&fs, f32, rgb, 0);
    uint32_t g    = spirv_composite_extract1(&fs, f32, rgb, 1);
    uint32_t b    = spirv_composite_extract1(&fs, f32, rgb, 2);
    uint32_t ro   = spirv_row3(&fs, f32, d, emf, epf, r, g, b);
    uint32_t go   = spirv_row3(&fs, f32, epf, d, emf, r, g, b);
    uint32_t bo   = spirv_row3(&fs, f32, emf, epf, d, r, g, b);
    uint32_t rgba = spirv_composite_construct4(&fs, v4, ro, go, bo, one);
    spirv_op_store(&fs, out_col, rgba);
    spirv_func_end(&fs);

    uint32_t fs_iface[] = {in_col, out_col};
    spirv_entry_point(&fs, SPIRV_EXECUTION_MODEL_FRAGMENT, main, "main", fs_iface, 2);
    spirv_exec_mode0(&fs, main, SPIRV_EXECUTION_MODE_ORIGIN_UPPER_LEFT);

    if (spirv_module_finish(&fs, out_fs, out_fs_n) != 0) {
        spirv_module_free(&fs);
        return -1;
    }

    spirv_dump_disk("/tmp/fs.spv", *out_fs, *out_fs_n);
    spirv_module_free(&fs);
    return 0;
}

// Note that Vulkan so debug-friendly that in case of SPIR-V typo (wrong index et cetera)
// it just segfaults at vkCreateGraphicsPipelines().
static int spirv_compile_shader(uint32_t** out_vs, uint32_t* out_vs_n, uint32_t** out_fs, uint32_t* out_fs_n)
{
#if DEMO_SHADERS_FROM_DISK
    spirv_read_disk("/home/fuck/git/RVVM/vulkan/draw/vert.spv", out_vs, out_vs_n);
    spirv_read_disk("/home/fuck/git/RVVM/vulkan/draw/frag.spv", out_fs, out_fs_n);
#else
    if (spirv_compile_triangle_vertex(out_vs, out_vs_n) < 0) {
        return -1;
    }
    if (spirv_compile_triangle_fragment(out_fs, out_fs_n) < 0) {
        return -1;
    }
#endif
    return 0;
}

#define W 800
#define H 600

typedef struct {
    SDL_Window*   win;
    SDL_Texture*  texture;
    SDL_Renderer* renderer;
} sdl_window_t;

static int sdl_create_window(sdl_window_t* win)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    win->win = SDL_CreateWindow("RVVM Vulkan RGB triangle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                SDL_WINDOW_SHOWN);
    if (!win->win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    // When SDL_RENDERER_ACCELERATED is enabled, it crashes Wayland compositor but
    // works under Xorg. I assume SDL then conflicts with our Vulkan driver
    // attempting to acquire same GPU as SDL does.
    win->renderer = SDL_CreateRenderer(win->win, -1, SDL_RENDERER_SOFTWARE);
    win->texture  = SDL_CreateTexture(win->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);

    return 0;
}

static void sdl_destroy_window(sdl_window_t* win)
{
    SDL_DestroyTexture(win->texture);
    SDL_DestroyRenderer(win->renderer);
    SDL_DestroyWindow(win->win);
    SDL_Quit();
}

int main(void)
{
    rvvm_set_loglevel(LOG_INFO);

    sdl_window_t win = {0};
    if (sdl_create_window(&win) < 0) {
        return -1;
    }

    gpu_vulkan_ctx_t* ctx = gpu_vulkan_create();
    if (!ctx) {
        fprintf(stderr, "gpu_vulkan_create failed\n");
        return 1;
    }

    // Generate shaders
    uint32_t* vs   = NULL;
    uint32_t* fs   = NULL;
    uint32_t  vs_n = 0;
    uint32_t  fs_n = 0;
    if (spirv_compile_shader(&vs, &vs_n, &fs, &fs_n) != 0) {
        fprintf(stderr, "SPIR-V generation failed\n");
        return 1;
    }

    triangle_frag_consts_t fragment_consts = {
        .hue = 0.0f,
    };

    gpu_vulkan_draw_t draw                           = {0};
    draw.stage[GPU_VULKAN_STAGE_VERTEX].spirv        = vs;
    draw.stage[GPU_VULKAN_STAGE_VERTEX].spirv_nwords = vs_n;

    draw.stage[GPU_VULKAN_STAGE_FRAGMENT].spirv        = fs;
    draw.stage[GPU_VULKAN_STAGE_FRAGMENT].spirv_nwords = fs_n;
    draw.stage[GPU_VULKAN_STAGE_FRAGMENT].constants    = &fragment_consts;
    draw.stage[GPU_VULKAN_STAGE_FRAGMENT].const_bytes  = sizeof(fragment_consts);

    draw.topology       = GPU_VULKAN_TOPOLOGY_TRIANGLE_LIST;
    draw.vertex_count   = 3;
    draw.instance_count = 1;
    draw.first_vertex   = 0;
    draw.first_instance = 0;

    bool draw_submitted = gpu_vulkan_submit_draw(ctx, &draw);
    if (!draw_submitted) {
        fprintf(stderr, "submit_draw failed\n");
        free(vs);
        free(fs);
        gpu_vulkan_destroy(ctx);
        return 1;
    }

    uint32_t   out_width  = 0;
    uint32_t   out_height = 0;
    uint32_t   out_stride = 0;
    rvvm_rgb_t out_format = 0;
    bool       have_frame = false;

    const uint32_t stride = (uint32_t)(W * 4);
    const size_t   bufsz  = (size_t)stride * H;
    uint8_t*       vram   = aligned_alloc(32, bufsz);
    if (!vram) {
        fprintf(stderr, "aligned_alloc failed\n");
        free(vs);
        free(fs);
        gpu_vulkan_destroy(ctx);
        return 1;
    }
    memset(vram, 0, bufsz);

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
        }

#define TAU 6.28318530718f
        // Advance the animation and hand the new value to the backend; the
        // next render tick picks it up. No draw resubmission involved.
        fragment_consts.hue += 0.05f;
        if (fragment_consts.hue >= TAU) {
            fragment_consts.hue -= TAU;
        }
        if (!gpu_vulkan_update_constants(ctx, GPU_VULKAN_STAGE_FRAGMENT, 0, &fragment_consts,
                                         sizeof(fragment_consts))) {
            fprintf(stderr, "update_constants failed\n");
        }

        uint32_t   out_width  = 0;
        uint32_t   out_height = 0;
        uint32_t   out_stride = 0;
        rvvm_rgb_t out_format = 0;

        if (draw_submitted) {
            void* pixels;
            int   pitch;
            if (SDL_LockTexture(win.texture, NULL, &pixels, &pitch) == 0) {
                if (gpu_vulkan_render_frame(ctx, W, H, pixels, bufsz, pitch, RVVM_RGB_XRGB8888, &out_width, &out_height,
                                            &out_stride, &out_format)
                    == 0) {
                    rvvm_warn("Failed to draw frame");
                }
                SDL_UnlockTexture(win.texture);
            }
        }

        SDL_RenderClear(win.renderer);
        SDL_RenderCopy(win.renderer, win.texture, NULL, NULL);
        SDL_RenderPresent(win.renderer);
        // Sleep ~16ms to acquire 60Hz rate ~> 1000/60.
        SDL_Delay(16);
    }

    free(vram);
    free(vs);
    free(fs);
    gpu_vulkan_destroy(ctx);
    sdl_destroy_window(&win);

    return 0;
}
