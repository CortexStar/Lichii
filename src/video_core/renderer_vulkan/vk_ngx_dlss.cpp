// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <fmt/format.h>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/neural_rendering/backend.h"
#include "video_core/neural_rendering/model.h"
#include "video_core/neural_rendering/runtime.h"
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/ngx_dlss.h"
#include "video_core/vulkan_common/nvidia_dlss.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace NR = VideoCore::NeuralRendering;
namespace {

/// Read an integer environment override, using the supplied fallback when the variable is
/// absent.
int EnvInt(const char *name, int fallback) {
    const char *value = std::getenv(name);
    return value ? std::atoi(value) : fallback;
}

/// Choose DLAA or a DLSS quality mode from the render-to-output width ratio.
NVSDK_NGX_PerfQuality_Value ModeFor(VkExtent2D render, VkExtent2D output) {
    const float ratio = static_cast<float>(render.width) / static_cast<float>(output.width);
    if (ratio >= 0.99f)
        return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (ratio >= 0.65f)
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio >= 0.57f)
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio >= 0.49f)
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

/// Read the supported DLSS preset override, defaulting to preset K.
unsigned Preset() {
    const char *value = std::getenv("EDEN_NX2_DLSS_PRESET");
    if (value && value[0] >= 'J' && value[0] <= 'M') {
        return NVSDK_NGX_DLSS_Hint_Render_Preset_J + value[0] - 'J';
    }
    if (value && std::string_view{value} == "default") {
        return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    }
    return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
}

NVSDK_NGX_Resource_VK Resource(const NvidiaDlssUpscaler::Image &image, VkExtent2D size,
                               bool write = false) {
    NVSDK_NGX_Resource_VK resource{};
    resource.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    resource.Resource.ImageViewInfo = {image.view,   image.image, image.range,
                                       image.format, size.width,  size.height};
    resource.ReadWrite = write;
    return resource;
}

VkImageMemoryBarrier Barrier(const NvidiaDlssUpscaler::Image &image, VkAccessFlags src,
                             VkAccessFlags dst, VkImageLayout before, VkImageLayout after) {
    auto range = image.range;
    range.aspectMask = image.barrier_aspect;
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = src,
            .dstAccessMask = dst,
            .oldLayout = before,
            .newLayout = after,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image.image,
            .subresourceRange = range};
}

} // namespace

/// Initialize the native NGX upscaler; NGX must already be available.
NvidiaDlssUpscaler::NvidiaDlssUpscaler(const Device &device_, Scheduler &scheduler_,
                                       MemoryAllocator &memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    if (!NvidiaDlss::IsAvailable()) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    CreateSampler();
}

/// Serialize device-idle waits and release neural/NGX resources after renderer recording has
/// stopped.
NvidiaDlssUpscaler::~NvidiaDlssUpscaler() {
    // Renderer shutdown has already stopped recording; do not submit to released game channels.
    std::scoped_lock queue_lock{scheduler.submit_mutex};
    void(device.GetLogical().WaitIdle());
    neural_rendering.reset();
    const auto &api = Ngx::GetApi();
    if (feature)
        api.release(feature);
    if (parameters)
        api.destroy_parameters(parameters);
}

/// Record native DLSS and optional neural rendering, recreating changed resources and reporting
/// fallback on failure.
bool NvidiaDlssUpscaler::Upscale(const Inputs &in) {
    if (failed) {
        return false;
    }
    const bool extent_changed = !output_image || in.render_size.width != render_extent.width ||
                                in.render_size.height != render_extent.height ||
                                in.upscale_size.width != upscale_extent.width ||
                                in.upscale_size.height != upscale_extent.height;
    const bool options_changed =
        extent_changed || in.hdr != hdr_input || in.depth_inverted != depth_inverted;
    if (options_changed && output_image) {
        scheduler.Finish();
        if (feature) {
            Ngx::GetApi().release(feature);
            feature = nullptr;
        }
    }
    if (extent_changed) {
        if (neural_rendering) {
            scheduler.Finish();
            std::scoped_lock queue_lock{scheduler.submit_mutex};
            neural_rendering.reset();
        }
        CreateOutput(in.upscale_size);
        render_extent = in.render_size;
        upscale_extent = in.upscale_size;
    }
    hdr_input = in.hdr;
    depth_inverted = in.depth_inverted;
    const auto neural_mode = Settings::GetNvidiaDlssRuntimeSettings().neural_rendering;
    const bool requested = device.SupportsNeuralRendering() &&
                           neural_mode != Settings::NeuralRendering::Off;
    const auto neural_style = requested ? static_cast<NR::Style>(static_cast<u32>(neural_mode) - 1)
                                       : NR::Style::Standard;
    PrepareNeuralRendering(in, requested);
    const bool hdr = in.hdr && EnvInt("EDEN_NX2_DLSS_HDR", 1) != 0;
    const bool auto_exposure = in.hdr && EnvInt("EDEN_NX2_DLSS_AUTO_EXPOSURE", 1) != 0;
    const auto mode = ModeFor(in.render_size, in.upscale_size);
    const auto preset = Preset();
    if (options_changed) {
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS (NGX): {}x{} -> {}x{}, mode {}, preset {}, {}",
                 in.render_size.width, in.render_size.height, in.upscale_size.width,
                 in.upscale_size.height, static_cast<int>(mode), preset, hdr ? "HDR" : "LDR");
    }
    const Image out{*output_view,
                    *output_image,
                    VK_FORMAT_R16G16B16A16_SFLOAT,
                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                    VK_IMAGE_ASPECT_COLOR_BIT};
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, out, hdr, auto_exposure, mode, preset, neural_style,
                      reset = in.reset || options_changed](vk::CommandBuffer cmdbuf) {
        const auto &api = Ngx::GetApi();
        const auto clear_output = [&] {
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                Barrier(out, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_GENERAL));
            cmdbuf.ClearColorImage(out.image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{},
                                   out.range);
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                Barrier(out, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL));
        };
        const auto fail = [&](const char *step, NVSDK_NGX_Result result) {
            if (!failed.exchange(true)) {
                Ngx::ReportFailure(fmt::format("{} failed ({:#x}); using the game's fallback", step,
                                               static_cast<u32>(result)));
            }
            clear_output();
        };
        if (failed) {
            clear_output();
            return;
        }
        // NGX is not thread-safe. Allocate, configure and evaluate on the scheduler worker;
        // feature release on the rendering thread follows Finish() or renderer shutdown.
        if (!parameters) {
            const auto result = api.allocate_parameters(&parameters);
            if (NVSDK_NGX_FAILED(result) || !parameters) {
                fail("parameter allocation", result);
                return;
            }
        }
        if (!feature) {
            parameters->Reset();
            parameters->Set(NVSDK_NGX_Parameter_Width, in.render_size.width);
            parameters->Set(NVSDK_NGX_Parameter_Height, in.render_size.height);
            parameters->Set(NVSDK_NGX_Parameter_OutWidth, in.upscale_size.width);
            parameters->Set(NVSDK_NGX_Parameter_OutHeight, in.upscale_size.height);
            parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(mode));
            int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            if (in.depth_inverted)
                flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
            if (hdr)
                flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
            if (auto_exposure)
                flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
            parameters->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
            parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
            parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
            for (const char *key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                                    NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality}) {
                parameters->Set(key, preset);
            }
            const auto result = api.create(*device.GetLogical(), *cmdbuf,
                                           NVSDK_NGX_Feature_SuperSampling, parameters, &feature);
            if (NVSDK_NGX_FAILED(result) || !feature) {
                fail("feature creation", result);
                return;
            }
        }
        auto color = Resource(in.color, in.render_size);
        auto depth = Resource(in.depth, in.render_size);
        auto motion = Resource(in.motion_vectors, in.render_size);
        auto output = Resource(out, in.upscale_size, true);
        parameters->Set(NVSDK_NGX_Parameter_Color, static_cast<void *>(&color));
        parameters->Set(NVSDK_NGX_Parameter_Depth, static_cast<void *>(&depth));
        parameters->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void *>(&motion));
        parameters->Set(NVSDK_NGX_Parameter_Output, static_cast<void *>(&output));
        parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, in.jitter[0]);
        parameters->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, in.jitter[1]);
        parameters->Set(NVSDK_NGX_Parameter_MV_Scale_X, in.motion_vector_scale[0]);
        parameters->Set(NVSDK_NGX_Parameter_MV_Scale_Y, in.motion_vector_scale[1]);
        parameters->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
        parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
                        in.render_size.width);
        parameters->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
                        in.render_size.height);
        parameters->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
        parameters->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
        constexpr VkAccessFlags writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                         VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array to_read{
            Barrier(in.color, writes, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, writes, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, writes, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(out, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);
        const auto result = api.evaluate(*cmdbuf, feature, parameters, nullptr);
        const std::array restore{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(out, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, restore);
        if (NVSDK_NGX_FAILED(result)) {
            fail("evaluation", result);
        } else {
            Ngx::ReportEvaluation();
            if (neural_rendering) {
                NR::Frame frame;
                frame.style = neural_style;
                frame.reset = reset;
                frame.hdr = in.hdr;
                frame.motion_scale = {in.motion_vector_scale[0] / in.render_size.width,
                                      in.motion_vector_scale[1] / in.render_size.height};
                neural_rendering->Record(*cmdbuf, frame);
                NR::ReportFrame();
            }
        }
    });
    scheduler.InvalidateState();
    return true;
}

/// Verify and initialize or rebind neural resources after draining work, serializing queue use
/// and failure cleanup.
void NvidiaDlssUpscaler::PrepareNeuralRendering(const Inputs& in, bool requested) {
    if (requested != neural_requested) {
        neural_requested = requested;
        neural_failed = false;
        if (!requested) {
            // Drain recorded uses before freeing the backend or clearing its status.
            scheduler.Finish();
            std::scoped_lock queue_lock{scheduler.submit_mutex};
            neural_rendering.reset();
            neural_motion_view = VK_NULL_HANDLE;
            NR::ReportInactive();
        }
    }
    if (!neural_requested || neural_failed ||
        (neural_rendering && neural_motion_view == in.motion_vectors.view)) {
        return;
    }
    // The backend uploads model constants during construction. Finish before touching its queue
    // or replacing descriptors referenced by work already recorded on the scheduler worker.
    scheduler.Finish();
    // The presentation thread also uses these queues. Keep construction, rebind failure cleanup
    // and their device-idle waits under the same lock, after Finish has released the worker.
    std::scoped_lock queue_lock{scheduler.submit_mutex};
    try {
        if (neural_rendering) {
            if (neural_rendering->HadTimeout()) {
                throw std::runtime_error("a neural rendering kernel timed out");
            }
            neural_rendering->Rebind(*output_image, *output_view, in.motion_vectors.view);
        } else {
            const auto model = NR::UserFolder();
            if (!neural_model_verified && !NR::IsInstalled(model)) {
                throw std::runtime_error(
                    "import the supported neural rendering model in Graphics settings");
            }
            neural_model_verified = true;
            neural_rendering = std::make_unique<NR::Backend>(
                device.GetInstance(), device.GetPhysical(), *device.GetLogical(),
                device.GetGraphicsFamily(), model, in.upscale_size, *output_image, *output_view,
                in.motion_vectors.view);
            LOG_INFO(Render_Vulkan, "Native FP8 neural rendering ready: {}x{}",
                     in.upscale_size.width, in.upscale_size.height);
            NR::ReportReady();
        }
        neural_motion_view = in.motion_vectors.view;
    } catch (const std::exception& error) {
        neural_rendering.reset();
        neural_failed = true;
        NR::ReportFailure(error.what());
    }
}

} // namespace Vulkan
