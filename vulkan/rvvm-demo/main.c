#include "devices/gpu-vulkan.h"
#include "gpu-vulkan-spirv.h"
#include "gpu-vulkan.h"
#include "utils.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Must match rvvm/rvvm_fb.h — wrong values break zero-copy / format mapping
 * in gpu_vulkan_native_format_for_guest(). */
#ifndef RVVM_RGB_RGB565
#define RVVM_RGB_RGB565      0x02
#define RVVM_RGB_XRGB8888    0x04
#define RVVM_RGB_XBGR8888    0x05
#define RVVM_RGB_XRGB2101010 0x06
#endif

static int make_rgb_triangle_spirv(uint32_t** out_vs, uint32_t* out_vs_n, uint32_t** out_fs, uint32_t* out_fs_n)
{
    // ---------- Vertex shader ----------
    spirv_module_t vs;
    spirv_module_init(&vs);
    spirv_module_begin(&vs);

    uint32_t void_ty = spirv_type_void(&vs);
    uint32_t f32     = spirv_type_float32(&vs);
    uint32_t i32     = spirv_type_int32(&vs);
    uint32_t v4      = spirv_type_vec4_float32(&vs);
    uint32_t bool_ty = spirv_type_bool(&vs);

    // Builtins
    uint32_t pos_ptr_ty = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_OUTPUT, v4);
    uint32_t pos_var    = spirv_global_var(&vs, pos_ptr_ty, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&vs, pos_var, SPIRV_DECORATION_BUILTIN, SPIRV_BUILTIN_POSITION);

    uint32_t vid_ptr_ty = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_INPUT, i32);
    uint32_t vid_var    = spirv_global_var(&vs, vid_ptr_ty, SPIRV_STORAGE_CLASS_INPUT);
    spirv_decorate_1(&vs, vid_var, SPIRV_DECORATION_BUILTIN, SPIRV_BUILTIN_VERTEX_INDEX);

    // Colour varying (location 0)
    uint32_t col_ptr_ty = spirv_type_ptr(&vs, SPIRV_STORAGE_CLASS_OUTPUT, v4);
    uint32_t col_var    = spirv_global_var(&vs, col_ptr_ty, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&vs, col_var, SPIRV_DECORATION_LOCATION, 0);

    // Uniform block (set 0, binding 0) – one vec4 used as a scale factor
    uint32_t elem_ptr;
    uint32_t ubo = spirv_uniform_vec4_array_block(&vs, 1, GPU_VULKAN_CONST_SET, 0, &elem_ptr);

    uint32_t fn_ty = spirv_type_func_void(&vs);
    uint32_t main  = spirv_func_begin(&vs, void_ty, fn_ty);

    // Load vertex index
    uint32_t vid = spirv_op_load(&vs, i32, vid_var);

    // Positions (NDC)
    uint32_t p0x = spirv_type_const_float32(&vs, -0.7f);
    uint32_t p0y = spirv_type_const_float32(&vs, -0.6f);
    uint32_t p1x = spirv_type_const_float32(&vs, 0.7f);
    uint32_t p1y = spirv_type_const_float32(&vs, -0.6f);
    uint32_t p2x = spirv_type_const_float32(&vs, 0.0f);
    uint32_t p2y = spirv_type_const_float32(&vs, 0.7f);
    uint32_t one = spirv_type_const_float32(&vs, 1.0f);
    uint32_t z0  = spirv_type_const_float32(&vs, 0.0f);

    // Select position by index (0/1/2)
    uint32_t c0  = spirv_type_const_int32(&vs, 0);
    uint32_t c1  = spirv_type_const_int32(&vs, 1);
    uint32_t eq0 = spirv_fcmp(&vs, SPIRV_OP_I_EQUAL, bool_ty, vid, c0);
    uint32_t eq1 = spirv_fcmp(&vs, SPIRV_OP_I_EQUAL, bool_ty, vid, c1);

    uint32_t sx = spirv_select(&vs, f32, eq0, p0x, spirv_select(&vs, f32, eq1, p1x, p2x));
    uint32_t sy = spirv_select(&vs, f32, eq0, p0y, spirv_select(&vs, f32, eq1, p1y, p2y));

    // Optional uniform scale (first component of the vec4)
    uint32_t scale_ptr = spirv_access_chain1(&vs, elem_ptr, ubo, spirv_type_const_uint32(&vs, 0));
    uint32_t scale     = spirv_op_load(&vs, f32, scale_ptr);
    sx                 = spirv_op_fmul(&vs, f32, sx, scale);
    sy                 = spirv_op_fmul(&vs, f32, sy, scale);

    uint32_t pos = spirv_composite_construct4(&vs, v4, sx, sy, z0, one);
    spirv_op_store(&vs, pos_var, pos);

    // Colours: red / green / blue
    uint32_t r = spirv_type_const_float32(&vs, 1.0f);
    uint32_t g = spirv_type_const_float32(&vs, 1.0f);
    uint32_t b = spirv_type_const_float32(&vs, 1.0f);
    uint32_t z = spirv_type_const_float32(&vs, 0.0f);

    uint32_t col0 = spirv_composite_construct4(&vs, v4, r, z, z, one); // red
    uint32_t col1 = spirv_composite_construct4(&vs, v4, z, g, z, one); // green
    uint32_t col2 = spirv_composite_construct4(&vs, v4, z, z, b, one); // blue

    uint32_t col = spirv_select(&vs, v4, eq0, col0, spirv_select(&vs, v4, eq1, col1, col2));
    spirv_op_store(&vs, col_var, col);

    spirv_func_end(&vs);

    uint32_t iface[] = {pos_var, col_var, vid_var};
    spirv_entry_point(&vs, SPIRV_EXECUTION_MODEL_VERTEX, main, "main", iface, 3);

    if (spirv_module_finish(&vs, out_vs, out_vs_n) != 0) {
        spirv_module_free(&vs);
        return -1;
    }
    spirv_module_free(&vs);

    // ---------- Fragment shader ----------
    spirv_module_t fs;
    spirv_module_init(&fs);
    spirv_module_begin(&fs);

    void_ty = spirv_type_void(&fs);
    f32     = spirv_type_float32(&fs);
    v4      = spirv_type_vec4_float32(&fs);

    // Input colour (location 0)
    uint32_t in_col_ptr = spirv_type_ptr(&fs, SPIRV_STORAGE_CLASS_INPUT, v4);
    uint32_t in_col     = spirv_global_var(&fs, in_col_ptr, SPIRV_STORAGE_CLASS_INPUT);
    spirv_decorate_1(&fs, in_col, SPIRV_DECORATION_LOCATION, 0);

    // Output FragColor (location 0)
    uint32_t out_col_ptr = spirv_type_ptr(&fs, SPIRV_STORAGE_CLASS_OUTPUT, v4);
    uint32_t out_col     = spirv_global_var(&fs, out_col_ptr, SPIRV_STORAGE_CLASS_OUTPUT);
    spirv_decorate_1(&fs, out_col, SPIRV_DECORATION_LOCATION, 0);

    fn_ty = spirv_type_func_void(&fs);
    main  = spirv_func_begin(&fs, void_ty, fn_ty);

    uint32_t c = spirv_op_load(&fs, v4, in_col);
    spirv_op_store(&fs, out_col, c);

    spirv_func_end(&fs);

    uint32_t fs_iface[] = {in_col, out_col};
    spirv_entry_point(&fs, SPIRV_EXECUTION_MODEL_FRAGMENT, main, "main", fs_iface, 2);
    spirv_exec_mode0(&fs, main, SPIRV_EXECUTION_MODE_ORIGIN_UPPER_LEFT);

    if (spirv_module_finish(&fs, out_fs, out_fs_n) != 0) {
        spirv_module_free(&fs);
        free(*out_vs);
        return -1;
    }
    spirv_module_free(&fs);
    return 0;
}

#define W 800
#define H 600

int main(void)
{
    rvvm_set_loglevel(LOG_INFO);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow("RVVM Vulkan RGB triangle", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                                       SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture*  tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);

    gpu_vulkan_ctx_t* ctx = gpu_vulkan_create();
    if (!ctx) {
        fprintf(stderr, "gpu_vulkan_create failed\n");
        return 1;
    }

    // Generate shaders
    uint32_t *vs = NULL, *fs = NULL;
    uint32_t  vs_n = 0, fs_n = 0;
    if (make_rgb_triangle_spirv(&vs, &vs_n, &fs, &fs_n) != 0) {
        fprintf(stderr, "SPIR-V generation failed\n");
        return 1;
    }

    // Constants: first float of the vertex uniform block = scale
    float consts[GPU_VULKAN_CONST_BYTES / 4] = {1.0f}; // scale = 1.0

    gpu_vulkan_draw_t draw                           = {0};
    draw.stage[GPU_VULKAN_STAGE_VERTEX].spirv        = vs;
    draw.stage[GPU_VULKAN_STAGE_VERTEX].spirv_nwords = vs_n;
    draw.stage[GPU_VULKAN_STAGE_VERTEX].constants    = consts;
    draw.stage[GPU_VULKAN_STAGE_VERTEX].const_bytes  = sizeof(float);

    draw.stage[GPU_VULKAN_STAGE_FRAGMENT].spirv        = fs;
    draw.stage[GPU_VULKAN_STAGE_FRAGMENT].spirv_nwords = fs_n;

    draw.topology       = GPU_VULKAN_TOPOLOGY_TRIANGLE_LIST;
    draw.vertex_count   = 3;
    draw.instance_count = 1;

    /* Mirror xe2: only mark the scene live after a successful submit.
     * Scanout/render_frame is then allowed to page-flip into vram. */
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

    /* Host-side scanout buffer — same role as rvvm_fbdev_get_vram() in xe2.
     * The worker blits directly into this; do not touch it from this thread
     * while a render may be in flight (no memset race). */
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

        /* xe2 only calls render_frame while draw_submitted is true. Out
         * params describe whatever the worker last finished writing — may
         * lag the requested size for a frame or two. */
        uint32_t   out_width  = 0;
        uint32_t   out_height = 0;
        uint32_t   out_stride = 0;
        rvvm_rgb_t out_format = 0;
        bool       have_frame = false;

        if (draw_submitted) {

            have_frame = gpu_vulkan_render_frame(ctx, W, H, vram, bufsz, stride, RVVM_RGB_XRGB8888, &out_width,
                                                 &out_height, &out_stride, &out_format);
        }

        if (have_frame && out_width && out_height && out_stride) {
            void* pixels;
            int   pitch;
            if (SDL_LockTexture(tex, NULL, &pixels, &pitch) == 0) {
                const uint32_t copy_w    = out_width < W ? out_width : W;
                const uint32_t copy_h    = out_height < H ? out_height : H;
                const size_t   row_bytes = (size_t)copy_w * 4;
                for (uint32_t y = 0; y < copy_h; y++) {
                    memcpy((uint8_t*)pixels + (size_t)y * (size_t)pitch, vram + (size_t)y * out_stride, row_bytes);
                }
                SDL_UnlockTexture(tex);
            }
        }

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        SDL_Delay(16); /* ~60 Hz, same cadence as xe2_update scanout */
    }

    free(vram);
    free(vs);
    free(fs);
    gpu_vulkan_destroy(ctx);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
