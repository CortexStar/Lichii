// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Linux: NVIDIA DLSS through NVIDIA NGX directly in place of a game's DLSS evaluation. Same
// class and behaviour as vk_streamline_dlss.cpp (Windows, Streamline).

#include <array>
#include <cstdlib>
#include <string_view>

#include "common/logging.h"
#include "video_core/dlss_evaluation.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_streamline_dlss.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;

int EnvInt(const char* name, int fallback) {
    const char* const value = std::getenv(name);
    return value ? std::atoi(value) : fallback;
}

// DLSS validates the render size against the mode's range: pick the mode for this ratio.
NVSDK_NGX_PerfQuality_Value ModeFor(VkExtent2D render, VkExtent2D upscale) {
    const f32 ratio = static_cast<f32>(render.width) / static_cast<f32>(upscale.width);
    if (ratio >= 0.99f) {
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
    if (ratio >= 0.65f) {
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
    if (ratio >= 0.57f) {
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    }
    if (ratio >= 0.49f) {
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    }
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

// Preset K for every mode, as on Windows. EDEN_NX2_DLSS_PRESET=J|K|L|M picks another model,
// EDEN_NX2_DLSS_PRESET=default leaves the choice to NVIDIA.
unsigned int Preset() {
    const char* const value = std::getenv("EDEN_NX2_DLSS_PRESET");
    if (value && value[0] >= 'J' && value[0] <= 'M') {
        return NVSDK_NGX_DLSS_Hint_Render_Preset_J + static_cast<unsigned int>(value[0] - 'J');
    }
    if (value && std::string_view{value} == "default") {
        return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    }
    return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
}

VkImageMemoryBarrier Barrier(const StreamlineDlssUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

NVSDK_NGX_Resource_VK Resource(const StreamlineDlssUpscaler::Image& image, VkExtent2D size) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range, image.format,
                                                  size.width, size.height, false);
}

} // Anonymous namespace

StreamlineDlssUpscaler::StreamlineDlssUpscaler(const Device& device_, Scheduler& scheduler_,
                                               MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    if (!Streamline::IsDlssAvailable()) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    NVSDK_NGX_Parameter* params{};
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&params)) || !params) {
        LOG_ERROR(Render_Vulkan, "NGX could not allocate DLSS parameters");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    ngx_params = params;
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

StreamlineDlssUpscaler::~StreamlineDlssUpscaler() {
    // Not scheduler.Finish(): see vk_streamline_dlss.cpp.
    void(device.GetLogical().WaitIdle());
    if (ngx_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(ngx_feature));
    }
    if (ngx_params) {
        NVSDK_NGX_VULKAN_DestroyParameters(static_cast<NVSDK_NGX_Parameter*>(ngx_params));
    }
}

void StreamlineDlssUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .pNext = nullptr,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange{
                                       .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                       .baseMipLevel = 0,
                                       .levelCount = 1,
                                       .baseArrayLayer = 0,
                                       .layerCount = 1,
                                   },
                               });
    });
}

bool StreamlineDlssUpscaler::Upscale(const Inputs& in) {
    if (failed) {
        return false;
    }
    const bool extent_changed = !output_image || in.render_size.width != render_extent.width ||
                                in.render_size.height != render_extent.height ||
                                in.upscale_size.width != upscale_extent.width ||
                                in.upscale_size.height != upscale_extent.height;
    const bool options_changed = extent_changed || in.hdr != hdr_input;
    const bool reset = in.reset || options_changed;
    if (options_changed && output_image) {
        // Recorded work may still create (and write ngx_feature) or use the old feature.
        scheduler.Finish();
    }
    if (options_changed && ngx_feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle*>(ngx_feature));
        ngx_feature = nullptr;
    }
    if (extent_changed) {
        CreateOutput(in.upscale_size);
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
    }
    hdr_input = in.hdr;

    const bool hdr = in.hdr && EnvInt("EDEN_NX2_DLSS_HDR", 1) != 0;
    const bool auto_exposure = in.hdr && EnvInt("EDEN_NX2_DLSS_AUTO_EXPOSURE", 1) != 0;
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    flags |= hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR : 0;
    flags |= auto_exposure ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0;
    flags |= in.depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0;
    const NVSDK_NGX_PerfQuality_Value mode = ModeFor(in.render_size, in.upscale_size);
    const unsigned int preset = Preset();
    if (options_changed) {
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS (NGX): {}x{} -> {}x{}, mode {}, preset {}, {}",
                 in.render_size.width, in.render_size.height, in.upscale_size.width,
                 in.upscale_size.height, static_cast<int>(mode), preset, hdr ? "HDR" : "LDR");
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, reset, flags, mode, preset, output_image_handle = *output_image,
                      output_view_handle = *output_view](vk::CommandBuffer cmdbuf) {
        auto* const params = static_cast<NVSDK_NGX_Parameter*>(ngx_params);
        const auto clear_output = [&] {
            const VkImageSubresourceRange range{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };
            const auto barrier = [&](VkAccessFlags src_access, VkAccessFlags dst_access) {
                return VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = src_access,
                    .dstAccessMask = dst_access,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = output_image_handle,
                    .subresourceRange = range,
                };
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   barrier(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                           VK_ACCESS_TRANSFER_WRITE_BIT));
            cmdbuf.ClearColorImage(output_image_handle, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{}, range);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                barrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
        };
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs come from the game's draws and dispatches and stay in GENERAL otherwise;
        // DLSS reads them as sampled images.
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        const VkMemoryBarrier output_written{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        NVSDK_NGX_Resource_VK color = Resource(in.color, in.render_size);
        NVSDK_NGX_Resource_VK depth = Resource(in.depth, in.render_size);
        NVSDK_NGX_Resource_VK motion = Resource(in.motion_vectors, in.render_size);
        const VkImageSubresourceRange output_range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        NVSDK_NGX_Resource_VK output = NVSDK_NGX_Create_ImageView_Resource_VK(
            output_view_handle, output_image_handle, output_range, OUTPUT_FORMAT,
            in.upscale_size.width, in.upscale_size.height, true);

        NVSDK_NGX_Result ngx_result = NVSDK_NGX_Result_Success;
        const auto result = VideoCommon::RecordDlssEvaluation(
            [&] {
                if (ngx_feature) {
                    return true;
                }
                for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                         NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                         NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                         NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                         NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                         NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
                    NVSDK_NGX_Parameter_SetUI(params, name, preset);
                }
                NVSDK_NGX_DLSS_Create_Params create{};
                create.Feature.InWidth = in.render_size.width;
                create.Feature.InHeight = in.render_size.height;
                create.Feature.InTargetWidth = in.upscale_size.width;
                create.Feature.InTargetHeight = in.upscale_size.height;
                create.Feature.InPerfQualityValue = mode;
                create.InFeatureCreateFlags = flags;
                NVSDK_NGX_Handle* handle{};
                ngx_result = NGX_VULKAN_CREATE_DLSS_EXT1(*device.GetLogical(), *cmdbuf, 1, 1,
                                                         &handle, params, &create);
                ngx_feature = handle;
                return NVSDK_NGX_SUCCEED(ngx_result) && handle;
            },
            [] { return true; },
            [&] {
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);
            },
            [] { return true; },
            [&] {
                NVSDK_NGX_VK_DLSS_Eval_Params eval{};
                eval.Feature.pInColor = &color;
                eval.Feature.pInOutput = &output;
                eval.pInDepth = &depth;
                eval.pInMotionVectors = &motion;
                eval.InJitterOffsetX = in.jitter[0];
                eval.InJitterOffsetY = in.jitter[1];
                eval.InRenderSubrectDimensions = {in.render_size.width, in.render_size.height};
                eval.InReset = reset ? 1 : 0;
                // Ours turn into render pixels, which is what NGX wants.
                eval.InMVScaleX = in.motion_vector_scale[0];
                eval.InMVScaleY = in.motion_vector_scale[1];
                ngx_result = NGX_VULKAN_EVALUATE_DLSS_EXT(
                    *cmdbuf, static_cast<NVSDK_NGX_Handle*>(ngx_feature), params, &eval);
                return NVSDK_NGX_SUCCEED(ngx_result);
            },
            [&] {
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, output_written, {},
                                       to_general);
            });
        if (result != VideoCommon::DlssEvaluationResult::Succeeded && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan,
                      "NVIDIA DLSS recording failed at stage {} (NGX result 0x{:08x}); using FSR 3.1",
                      static_cast<int>(result), static_cast<u32>(ngx_result));
        }
        if (result != VideoCommon::DlssEvaluationResult::Succeeded) {
            clear_output();
        }
    });
    // DLSS binds its own pipelines and descriptors on the command buffer.
    scheduler.InvalidateState();
    return true;
}

} // namespace Vulkan
