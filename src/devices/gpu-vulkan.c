/*
gpu-vulkan.c - Vulkan GPU backend
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "gpu-vulkan.h"
#include "__vulkan_snippet_frag.h"
#include "__vulkan_snippet_vert.h"
#include "utils.h"

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XE2_VK_COLOR_FORMAT VK_FORMAT_B8G8R8A8_UNORM

// Log-and-bail helper. Every Vulkan call site uses this instead of
// aborting the process - a broken GPU driver should not take the whole
// emulator down with it.
#define VK_TRY(x) do { \
        VkResult res__ = (x); \
        if (res__ != VK_SUCCESS) { \
            rvvm_warn("%s failed (%d) at %s:%d\n", #x, res__, __FILE__, __LINE__); \
            goto fail; \
        } \
    } while (0)

struct gpu_vulkan_ctx_t {
    VkInstance       instance;
    VkPhysicalDevice physical_device;
    VkDevice         device;
    uint32_t         graphics_family;
    VkQueue          graphics_queue;

    VkRenderPass     render_pass;
    VkPipelineLayout pipeline_layout;
    VkPipeline       pipeline;

    VkCommandPool    command_pool;
    VkCommandBuffer  command_buffer;
    VkFence          fence;

    // Per-resolution resources; recreated only when width/height change.
    uint32_t         cur_width;
    uint32_t         cur_height;
    VkImage          color_image;
    VkDeviceMemory   color_memory;
    VkImageView      color_view;
    VkFramebuffer    framebuffer;
    VkBuffer         readback_buffer;
    VkDeviceMemory   readback_memory;
    void            *readback_mapped;

    uint64_t         frame_index;
};



// -----------------------------------------------------------
// Initialization
// -----------------------------------------------------------



static bool gpu_vulkan_create_instance_and_device(gpu_vulkan_ctx_t *ctx)
{
    VkApplicationInfo app_info = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xe2-scanout-renderer",
        .apiVersion       = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo inst_ci = {
        .sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        // No extensions: no surface/presentation is ever used.
    };
    VK_TRY(vkCreateInstance(&inst_ci, NULL, &ctx->instance));

    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &dev_count, NULL);
    if (dev_count == 0) {
        rvvm_warn("No Vulkan devices available\n");
        goto fail;
    }
    VkPhysicalDevice *devices = safe_malloc(sizeof(VkPhysicalDevice) * dev_count);
    vkEnumeratePhysicalDevices(ctx->instance, &dev_count, devices);

    int graphics_family = -1;
    for (uint32_t i = 0; i < dev_count && graphics_family < 0; i++) {
        uint32_t q_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &q_count, NULL);
        VkQueueFamilyProperties *q_props = malloc(sizeof(VkQueueFamilyProperties) * q_count);
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &q_count, q_props);
        for (uint32_t q = 0; q < q_count; q++) {
            if (q_props[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                ctx->physical_device = devices[i];
                graphics_family = (int)q;
                break;
            }
        }
        free(q_props);
    }
    free(devices);

    if (graphics_family < 0) {
        rvvm_warn("No graphics-capable queue family found\n");
        goto fail;
    }
    ctx->graphics_family = (uint32_t)graphics_family;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->graphics_family,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dev_ci = {
        .sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos    = &queue_ci,
        // No VK_KHR_swapchain - nothing here ever presents.
    };
    VK_TRY(vkCreateDevice(ctx->physical_device, &dev_ci, NULL, &ctx->device));
    vkGetDeviceQueue(ctx->device, ctx->graphics_family, 0, &ctx->graphics_queue);
    return true;

fail:
    return false;
}

static bool gpu_vulkan_create_render_pass(gpu_vulkan_ctx_t *ctx)
{
    VkAttachmentDescription color_attachment = {
        .format         = XE2_VK_COLOR_FORMAT,
        .samples        = VK_SAMPLE_COUNT_1_BIT,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        // Land directly in a copy-source layout: no manual barrier needed
        // before vkCmdCopyImageToBuffer after the render pass ends.
        .finalLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    VkAttachmentReference color_ref = {
        .attachment     = 0,
        .layout         = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &color_ref,
    };
    VkSubpassDependency dependency = {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo ci = {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &color_attachment,
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dependency,
    };
    VK_TRY(vkCreateRenderPass(ctx->device, &ci, NULL, &ctx->render_pass));
    return true;
fail:
    return false;
}

// On pipeline configuration: now moved to initialization stage, as
// shaders are hardcoded for tests sake. But when guest reports a new
// shader via Xe2 assembly, we should load/unload shader modules as
// they arrive or leave.
static bool gpu_vulkan_create_pipeline(gpu_vulkan_ctx_t *ctx)
{
    VkShaderModuleCreateInfo vertex_ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(xe2_vert_spv),
        .pCode    = xe2_vert_spv,
    };
    VkShaderModuleCreateInfo fragment_ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(xe2_frag_spv),
        .pCode    = xe2_frag_spv,
    };
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VK_TRY(vkCreateShaderModule(ctx->device, &vertex_ci, NULL, &vertex_module));
    VK_TRY(vkCreateShaderModule(ctx->device, &fragment_ci, NULL, &fragment_module));

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_module,
            .pName  = "main"
        },
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_module,
            .pName  = "main"
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };

    // Viewport/scissor are dynamic so the same pipeline serves any
    // plane resolution the guest sets up - no pipeline recreation on
    // a mode change, only the offscreen image/framebuffer.
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkDynamicState dynamic_states[2] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates    = dynamic_states,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT
                        | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT
                        | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_attachment,
    };

    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset     = 0,
        .size       = sizeof(float),
    };
    VkPipelineLayoutCreateInfo layout_ci = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &push_range,
    };
    VK_TRY(vkCreatePipelineLayout(ctx->device, &layout_ci, NULL, &ctx->pipeline_layout));

    VkGraphicsPipelineCreateInfo pipeline_ci = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState      = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState   = &multisampling,
        .pColorBlendState    = &color_blending,
        .pDynamicState       = &dynamic_state,
        .layout              = ctx->pipeline_layout,
        .renderPass          = ctx->render_pass,
        .subpass             = 0,
    };
    VK_TRY(vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &pipeline_ci, NULL, &ctx->pipeline));

    vkDestroyShaderModule(ctx->device, fragment_module, NULL);
    vkDestroyShaderModule(ctx->device, vertex_module, NULL);
    return true;

fail:
    if (fragment_module) {
        vkDestroyShaderModule(ctx->device, fragment_module, NULL);
    }
    if (vertex_module) {
        vkDestroyShaderModule(ctx->device, vertex_module, NULL);
    }
    return false;
}

static bool gpu_vulkan_create_command_and_sync(gpu_vulkan_ctx_t *ctx)
{
    VkCommandPoolCreateInfo pool_ci = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->graphics_family,
    };
    VK_TRY(vkCreateCommandPool(ctx->device, &pool_ci, NULL, &ctx->command_pool));

    VkCommandBufferAllocateInfo alloc_info = {
        .sType       = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level       = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_TRY(vkAllocateCommandBuffers(ctx->device, &alloc_info, &ctx->command_buffer));

    VkFenceCreateInfo fence_ci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };
    VK_TRY(vkCreateFence(ctx->device, &fence_ci, NULL, &ctx->fence));
    return true;
fail:
    return false;
}

gpu_vulkan_ctx_t *gpu_vulkan_create(void)
{
    gpu_vulkan_ctx_t *ctx = safe_calloc(1, sizeof(*ctx));

    if (!gpu_vulkan_create_instance_and_device(ctx)) {
        goto fail;
    }
    if (!gpu_vulkan_create_render_pass(ctx)) {
        goto fail;
    }
    if (!gpu_vulkan_create_pipeline(ctx)) {
        goto fail;
    }
    if (!gpu_vulkan_create_command_and_sync(ctx)) {
        goto fail;
    }

    return ctx;

fail:
    gpu_vulkan_destroy(ctx);
    return NULL;
}

static void gpu_vulkan_destroy_sized_resources(gpu_vulkan_ctx_t *ctx)
{
    if (!ctx->device) {
        return;
    }
    if (ctx->framebuffer) {
        vkDestroyFramebuffer(ctx->device, ctx->framebuffer, NULL);
    }
    if (ctx->color_view) {
        vkDestroyImageView(ctx->device, ctx->color_view, NULL);
    }
    if (ctx->color_image) {
        vkDestroyImage(ctx->device, ctx->color_image, NULL);
    }
    if (ctx->color_memory) {
        vkFreeMemory(ctx->device, ctx->color_memory, NULL);
    }
    if (ctx->readback_mapped) {
        vkUnmapMemory(ctx->device, ctx->readback_memory);
    }
    if (ctx->readback_buffer) {
        vkDestroyBuffer(ctx->device, ctx->readback_buffer, NULL);
    }
    if (ctx->readback_memory) {
        vkFreeMemory(ctx->device, ctx->readback_memory, NULL);
    }
    ctx->framebuffer = VK_NULL_HANDLE;
    ctx->color_view = VK_NULL_HANDLE;
    ctx->color_image = VK_NULL_HANDLE;
    ctx->color_memory = VK_NULL_HANDLE;
    ctx->readback_buffer = VK_NULL_HANDLE;
    ctx->readback_memory = VK_NULL_HANDLE;
    ctx->readback_mapped = NULL;
}

void gpu_vulkan_destroy(gpu_vulkan_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->device) {
        vkDeviceWaitIdle(ctx->device);
    }

    gpu_vulkan_destroy_sized_resources(ctx);

    if (ctx->fence) {
        vkDestroyFence(ctx->device, ctx->fence, NULL);
    }
    if (ctx->command_pool) {
        vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
    }
    if (ctx->pipeline) {
        vkDestroyPipeline(ctx->device, ctx->pipeline, NULL);
    }
    if (ctx->pipeline_layout) {
        vkDestroyPipelineLayout(ctx->device, ctx->pipeline_layout, NULL);
    }
    if (ctx->render_pass) {
        vkDestroyRenderPass(ctx->device, ctx->render_pass, NULL);
    }
    if (ctx->device) {
        vkDestroyDevice(ctx->device, NULL);
    }
    if (ctx->instance) {
        vkDestroyInstance(ctx->instance, NULL);
    }
    free(ctx);
}



// -----------------------------------------------------------
// Real-time render
// -----------------------------------------------------------



static uint32_t gpu_vulkan_find_memory_type(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props = {0};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

// This optionally re-maps Vulkan framebuffer to CPU-visible memory.
static bool gpu_vulkan_resize_targets(gpu_vulkan_ctx_t *ctx, uint32_t w, uint32_t h)
{
    if (ctx->framebuffer != VK_NULL_HANDLE && ctx->cur_width == w && ctx->cur_height == h) {
        return true; // Already sized correctly - typical steady-state case.
    }

    gpu_vulkan_destroy_sized_resources(ctx);

    // Offscreen color image.
    VkImageCreateInfo image_ci = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = XE2_VK_COLOR_FORMAT,
        .extent        = {
            .width  = w,
            .height = h,
            .depth  = 1
        },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_TRY(vkCreateImage(ctx->device, &image_ci, NULL, &ctx->color_image));

    VkMemoryRequirements image_req = {0};
    vkGetImageMemoryRequirements(ctx->device, ctx->color_image, &image_req);
    uint32_t image_mem_type = gpu_vulkan_find_memory_type(ctx->physical_device, image_req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (image_mem_type == UINT32_MAX) {
        goto fail;
    }
    VkMemoryAllocateInfo image_alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = image_req.size,
        .memoryTypeIndex = image_mem_type,
    };
    VK_TRY(vkAllocateMemory(ctx->device, &image_alloc, NULL, &ctx->color_memory));
    VK_TRY(vkBindImageMemory(ctx->device, ctx->color_image, ctx->color_memory, 0));

    VkImageViewCreateInfo view_ci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = ctx->color_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = XE2_VK_COLOR_FORMAT,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1
        },
    };
    VK_TRY(vkCreateImageView(ctx->device, &view_ci, NULL, &ctx->color_view));

    VkFramebufferCreateInfo framebuffer_ci = {
        .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass      = ctx->render_pass,
        .attachmentCount = 1,
        .pAttachments    = &ctx->color_view,
        .width           = w,
        .height          = h,
        .layers          = 1,
    };
    VK_TRY(vkCreateFramebuffer(ctx->device, &framebuffer_ci, NULL, &ctx->framebuffer));

    // Host-visible readback buffer: tightly packed width * height * 4 bytes.
    VkDeviceSize buf_size = (VkDeviceSize) w * h * 4;
    VkBufferCreateInfo buf_ci = {
        .size        = buf_size,
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_TRY(vkCreateBuffer(ctx->device, &buf_ci, NULL, &ctx->readback_buffer));

    VkMemoryRequirements buf_req = {0};
    vkGetBufferMemoryRequirements(ctx->device, ctx->readback_buffer, &buf_req);
    uint32_t buf_mem_type = gpu_vulkan_find_memory_type(ctx->physical_device, buf_req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_mem_type == UINT32_MAX) {
        goto fail;
    }
    VkMemoryAllocateInfo buf_alloc = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = buf_req.size,
        .memoryTypeIndex = buf_mem_type,
    };
    VK_TRY(vkAllocateMemory(ctx->device, &buf_alloc, NULL, &ctx->readback_memory));
    VK_TRY(vkBindBufferMemory(ctx->device, ctx->readback_buffer, ctx->readback_memory, 0));
    // HOST_COHERENT: no explicit flush/invalidate needed around reads.
    VK_TRY(vkMapMemory(ctx->device, ctx->readback_memory, 0, buf_size, 0, &ctx->readback_mapped));

    ctx->cur_width = w;
    ctx->cur_height = h;
    return true;

fail:
    gpu_vulkan_destroy_sized_resources(ctx);
    return false;
}

bool gpu_vulkan_render_frame(gpu_vulkan_ctx_t *ctx, uint32_t width, uint32_t height,
                          const uint8_t **out_pixels, size_t *out_row_pitch)
{
    if (!ctx || width == 0 || height == 0) {
        return false;
    }
    if (!gpu_vulkan_resize_targets(ctx, width, height)) {
        return false;
    }

    // Deterministic "one frame per scanout call" clock, per the prompt's
    // framing - not tied to wall-clock time.
    const float dt = 1.0f / 60.0f;
    const float speed = 1.5f;
    float time = (float) ctx->frame_index * dt * speed;

    vkResetCommandBuffer(ctx->command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    VK_TRY(vkBeginCommandBuffer(ctx->command_buffer, &begin_info));

    VkViewport viewport = {
        .x        = 0,
        .y        = 0,
        .width    = (float) width,
        .height   = (float) height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    VkRect2D scissor = {
        .offset = {
            .x = 0,
            .y = 0
        },
        .extent = {
            .width  = width,
            .height = height
        }
    };
    vkCmdSetViewport(ctx->command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(ctx->command_buffer, 0, 1, &scissor);

    VkClearValue clear = {
        .color = {
            .float32 = { 0.02f, 0.02f, 0.05f, 1.0f }
        }
    };
    VkRenderPassBeginInfo render_pass_info = {
        .sType        = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass   = ctx->render_pass,
        .framebuffer  = ctx->framebuffer,
        .renderArea   = {
            .offset = {
                .x = 0,
                .y = 0
            },
            .extent = {
                .width  = width,
                .height = height
            }
        },
        .clearValueCount = 1,
        .pClearValues    = &clear,
    };
    vkCmdBeginRenderPass(ctx->command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(ctx->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->pipeline);
    // Pass constant to a compiled SPIR-V shader. It would be visible through:
    //
    // layout(push_constant) uniform PushConsts {
    //     float time;
    // } pc;
    vkCmdPushConstants(ctx->command_buffer, ctx->pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(float), &time);
    // Our hardcoded shader sets 3 vertices, so we pass vertexCount=3.
    // In case of own SPIR-V compiler we need to maintain this kind of
    // metadata along with the compiled shader itself.
    vkCmdDraw(ctx->command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(ctx->command_buffer);
    // renderPass finalLayout already leaves the image in TRANSFER_SRC_OPTIMAL.

    VkBufferImageCopy region = {
        .bufferOffset      = 0,
        .bufferRowLength   = width,
        .bufferImageHeight = height,
        .imageSubresource  = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel       = 0,
            .baseArrayLayer = 0,
            .layerCount     = 1
        },
        .imageExtent = {
            .width  = width,
            .height = height,
            .depth  = 1
        },
    };
    vkCmdCopyImageToBuffer(ctx->command_buffer, ctx->color_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            ctx->readback_buffer, 1, &region);

    VK_TRY(vkEndCommandBuffer(ctx->command_buffer));

    VK_TRY(vkResetFences(ctx->device, 1, &ctx->fence));
    VkSubmitInfo submit = {
        .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers    = &ctx->command_buffer,
    };
    VK_TRY(vkQueueSubmit(ctx->graphics_queue, 1, &submit, ctx->fence));

    // Blocking wait, matching the synchronous style of the existing DMA
    // copy loop in xe2_scanout(). A single triangle draw completes in
    // well under a millisecond on any real GPU.
    VK_TRY(vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX));

    *out_pixels = (const uint8_t *) ctx->readback_mapped;
    *out_row_pitch = (size_t) width * 4;
    ctx->frame_index++;
    return true;

fail:
    return false;
}
