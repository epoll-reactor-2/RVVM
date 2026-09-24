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
// layout(location = 0) in vec3 inPosition; // binding 0 - see triangle_positions[] below.
// layout(location = 1) in vec3 inColor;    // binding 1 - see triangle_colors[] below.
//
// layout(location = 0) out vec3 fragColor;
//
// void main() {
//     gl_Position = vec4(inPosition, 1.0);
//     fragColor = inColor;
// }
static int spirv_compile_triangle_vertex(uint32_t** out_vs, uint32_t* out_vs_n)
{
    spirv_module_t vs = {0};
    spirv_module_init(&vs);
    spirv_module_begin(&vs);

    uint32_t void_ty = spirv_type_void(&vs);
    uint32_t f32     = spirv_type_float32(&vs);
    uint32_t v3      = spirv_type_vec3_float32(&vs);
    uint32_t v4      = spirv_type_vec4_float32(&vs);
    uint32_t fn_ty   = spirv_type_func_void(&vs);

    uint32_t pos_ptr = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_OUTPUT, v4);
    uint32_t pos_var = spirv_global_var(&vs, pos_ptr, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&vs, pos_var, SPIRV_DECORATION_BUILTIN, SPIRV_BUILTIN_POSITION);
    uint32_t col_ptr = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_OUTPUT, v3);
    uint32_t col_var = spirv_global_var(&vs, col_ptr, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&vs, col_var, SPIRV_DECORATION_LOCATION, 0);

    // Vertex attribute inputs. Locations/format here must match
    // draw.vertex.attrib[] in main() below.
    uint32_t in_pos_var = spirv_declare_location_in(&vs, 0, v3, "inPosition");
    uint32_t in_col_var = spirv_declare_location_in(&vs, 1, v3, "inColor");

    uint32_t main = spirv_func_begin(&vs, void_ty, fn_ty);
    spirv_name(&vs, main, "main");

    // gl_Position = vec4(inPosition, 1.0)
    uint32_t p  = spirv_op_load(&vs, v3, in_pos_var);
    uint32_t x  = spirv_composite_extract1(&vs, f32, p, 0);
    uint32_t y  = spirv_composite_extract1(&vs, f32, p, 1);
    uint32_t z  = spirv_composite_extract1(&vs, f32, p, 2);
    uint32_t w1 = spirv_type_const_float32(&vs, 1.0f);
    spirv_op_store(&vs, pos_var, spirv_composite_construct4(&vs, v4, x, y, z, w1));

    // fragColor = inColor
    spirv_op_store(&vs, col_var, spirv_op_load(&vs, v3, in_col_var));

    spirv_func_end(&vs);

    uint32_t vs_iface[] = {pos_var, col_var, in_pos_var, in_col_var};
    spirv_entry_point(&vs, SPIRV_EXECUTION_MODEL_VERTEX, main, "main", vs_iface, 4);

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
    uint32_t v3      = spirv_type_vec3_float32(&fs);

    uint32_t out_col = spirv_declare_location_out(&fs, 0, v4, "outColor");
    uint32_t in_col  = spirv_declare_location_in(&fs, 0, v3, "fragColor");

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

// Two separate bindings, one per attribute - deliberately not
// interleaved into one struct, so this exercises the same
// multiple-VkVertexInputBindingDescription path a real multi-buffer
// guest draw does (see gpu_vulkan_vertex_attrib_t.binding in
// gpu-vulkan.h). An interleaved single buffer works too; just give
// every attribute the same binding index and vary .offset instead.
static const float triangle_positions[][3] = {
    { 0.0f, -0.5f, 0.0f},
    { 0.5f,  0.5f, 0.0f},
    {-0.5f,  0.5f, 0.0f},
    {-0.1f,  0.2f, 0.3f},
};
static const float triangle_colors[][3] = {
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, 1.0f},
};

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

#define STAR_POINTS 30

static float star_positions[STAR_POINTS][3];
static float star_colors[STAR_POINTS][3];

void build_star(void)
{
    const float R = 0.55f, r = 0.22f;
    const float cx = 0.f, cy = 0.f;
    int         v = 0;
    for (int i = 0; i < 5; i++) {
        float a0 = -1.5708f + i * 1.2566f;
        float a1 = a0 + 0.6283f;
        float a2 = a0 + 1.2566f;

        star_positions[v][0] = cx + R * cosf(a0);
        star_positions[v][1] = cy + R * sinf(a0);
        star_positions[v][2] = 0;
        star_colors[v][0]    = 1;
        star_colors[v][1]    = 0.8f;
        star_colors[v][2]    = 0.2f;
        v++;
        star_positions[v][0] = cx + r * cosf(a1);
        star_positions[v][1] = cy + r * sinf(a1);
        star_positions[v][2] = 0;
        star_colors[v][0]    = 1;
        star_colors[v][1]    = 0.4f;
        star_colors[v][2]    = 0.1f;
        v++;
        star_positions[v][0] = cx;
        star_positions[v][1] = cy;
        star_positions[v][2] = 0;
        star_colors[v][0]    = 1;
        star_colors[v][1]    = 1;
        star_colors[v][2]    = 0.6f;
        v++;

        star_positions[v][0] = cx + r * cosf(a1);
        star_positions[v][1] = cy + r * sinf(a1);
        star_positions[v][2] = 0;
        star_colors[v][0]    = 1;
        star_colors[v][1]    = 0.4f;
        star_colors[v][2]    = 0.1f;
        v++;
        star_positions[v][0] = cx + R * cosf(a2);
        star_positions[v][1] = cy + R * sinf(a2);
        star_positions[v][2] = 0;
        star_colors[v][0]    = 1;
        star_colors[v][1]    = 0.8f;
        star_colors[v][2]    = 0.2f;
        v++;
        star_positions[v][0] = cx;
        star_positions[v][1] = cy;
        star_positions[v][2] = 0;
        star_colors[v][0]    = 1;
        star_colors[v][1]    = 1;
        star_colors[v][2]    = 0.6f;
        v++;
    }
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

#define STERN
#ifdef STERN
    build_star();

    draw.vertex.binding[0] = (gpu_vulkan_vertex_binding_t) {
        .data   = star_positions,
        .size   = sizeof(star_positions),
        .stride = 3 * sizeof(float),
    };
    draw.vertex.binding[1] = (gpu_vulkan_vertex_binding_t) {
        .data   = star_colors,
        .size   = sizeof(star_colors),
        .stride = 3 * sizeof(float),
    };
    draw.vertex.binding_count = 2;

    draw.vertex.attrib[0] = (gpu_vulkan_vertex_attrib_t) {
        .location = 0,
        .binding  = 0,
        .format   = GPU_VULKAN_FORMAT_R32G32B32_SFLOAT,
        .offset   = 0,
    };
    draw.vertex.attrib[1] = (gpu_vulkan_vertex_attrib_t) {
        .location = 1,
        .binding  = 1,
        .format   = GPU_VULKAN_FORMAT_R32G32B32_SFLOAT,
        .offset   = 0,
    };
    draw.vertex.attrib_count = 2;
    static uint16_t star_indices[STAR_POINTS * 3];
    for (int i = 0; i < STAR_POINTS; i++) {
        star_indices[i * 3 + 0] = 0;
        star_indices[i * 3 + 1] = 1 + i * 2;
        star_indices[i * 3 + 2] = 1 + (i * 2 + 1) % (STAR_POINTS * 2);
    }
    draw.topology       = GPU_VULKAN_TOPOLOGY_TRIANGLE_LIST;
    draw.vertex_count   = STAR_POINTS * 2 + 1;
    draw.instance_count = 1;
    draw.first_vertex   = 0;

    draw.vertex.index_type = 0;               // UINT16 or whatever the header defines
    draw.vertex_count      = STAR_POINTS * 3; // number of indices
#else
    draw.topology       = GPU_VULKAN_TOPOLOGY_TRIANGLE_LIST;
    draw.vertex_count   = STATIC_ARRAY_SIZE(triangle_positions);
    draw.instance_count = 1;
    draw.first_vertex   = 0;
    draw.first_instance = 0;

    // Vertex input: two bindings (positions, colors), one attribute
    // each. Everything here is copied by gpu_vulkan_submit_draw() below,
    // so these arrays don't need to outlive the call - they're static
    // only because that's the simplest way to define constant data.
    draw.vertex.binding[0] = (gpu_vulkan_vertex_binding_t) {
        .data   = triangle_positions,
        .size   = sizeof(triangle_positions),
        .stride = sizeof(triangle_positions[0]),
    };
    draw.vertex.binding[1] = (gpu_vulkan_vertex_binding_t) {
        .data   = triangle_colors,
        .size   = sizeof(triangle_colors),
        .stride = sizeof(triangle_colors[0]),
    };
    draw.vertex.binding_count = 2;

    draw.vertex.attrib[0] = (gpu_vulkan_vertex_attrib_t) {
        .location = 0,
        .binding  = 0,
        .format   = GPU_VULKAN_FORMAT_R32G32B32_SFLOAT,
        .offset   = 0,
    };
    draw.vertex.attrib[1] = (gpu_vulkan_vertex_attrib_t) {
        .location = 1,
        .binding  = 1,
        .format   = GPU_VULKAN_FORMAT_R32G32B32_SFLOAT,
        .offset   = 0,
    };
    draw.vertex.attrib_count = 2;
#endif /* STERN */

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
