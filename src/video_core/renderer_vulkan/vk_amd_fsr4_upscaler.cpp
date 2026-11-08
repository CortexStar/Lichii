// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_amd_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/amd_ffx.h"
#include "video_core/vulkan_common/amd_ffx_api.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

/// AMD's Vulkan backend keeps FFX_MAX_QUEUED_FRAMES (4) frames of constants and views: keep at
/// most this many dispatches on the GPU (bloodborne_pc's vk_fsr_amd.cpp keeps 3 the same way).
constexpr size_t MAX_DISPATCHES_IN_FLIGHT = 3;

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat EXPOSURE_FORMAT = VK_FORMAT_R32_SFLOAT;
constexpr VkImageSubresourceRange COLOR_RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

VkDeviceContext DeviceContext(const Device& device) {
    return VkDeviceContext{
        .vkDevice = *device.GetLogical(),
        .vkPhysicalDevice = device.GetPhysical(),
        .vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr,
    };
}

/// The newest FSR 4 (major version 4 or newer) the runtime offers for this GPU.
const AmdFfx::Version* NewestFsr4(const std::vector<AmdFfx::Version>& versions) {
    std::vector<std::string> names;
    names.reserve(versions.size());
    for (const auto& version : versions) {
        names.push_back(version.name);
    }
    const auto newest = AmdFfx::NewestVersion4(names);
    return newest ? &versions[*newest] : nullptr;
}

/// EDEN_FFX_UPSCALER=<part of a version name> pins that version (an A/B switch); the default is
/// the newest FSR 4.
std::optional<u64> VersionOverride(const std::vector<AmdFfx::Version>& versions) {
    const char* const wanted = std::getenv("EDEN_FFX_UPSCALER");
    if (wanted != nullptr && *wanted != '\0') {
        for (const auto& version : versions) {
            if (version.name.find(wanted) != std::string::npos) {
                return version.id;
            }
        }
        LOG_WARNING(Render_Vulkan, "AMD FSR: no upscaler version matches EDEN_FFX_UPSCALER={}",
                    wanted);
    }
    if (const AmdFfx::Version* const newest = NewestFsr4(versions)) {
        return newest->id;
    }
    return std::nullopt;
}

} // Anonymous namespace

struct AmdFsr4Upscaler::Context {
    ffxContext context{};
    VkExtent2D max_render{};
    VkExtent2D upscale{};
    bool hdr{};
    std::string provider;
    std::string error;
};

AmdFsr4Upscaler::AmdFsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                                 MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {}

AmdFsr4Upscaler::~AmdFsr4Upscaler() {
    if (staging_thread.joinable()) {
        staging_thread.join();
    }
    Destroy(staging);
    Destroy(current);
}

bool AmdFsr4Upscaler::IsAvailable(const Device& device) {
    static const bool available = [&device] {
        if (!AmdFfx::HasUpscaler()) {
            return false;
        }
        VkDeviceContext context = DeviceContext(device);
        const auto versions = AmdFfx::QueryVersions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
                                                    context);
        std::string list;
        for (const auto& version : versions) {
            list += (list.empty() ? "" : ", ") + version.name;
        }
        LOG_INFO(Render_Vulkan, "AMD FSR: upscaler versions for this GPU: {}",
                 list.empty() ? "none" : list);
        if (!NewestFsr4(versions)) {
            LOG_INFO(Render_Vulkan, "AMD FSR: no FSR 4 upscaler for this GPU; using FSR 3.1");
            return false;
        }
        return true;
    }();
    return available;
}

void AmdFsr4Upscaler::Destroy(std::unique_ptr<Context>& context) {
    if (!context) {
        return;
    }
    if (context->context != nullptr) {
        // The GPU and the scheduler's worker may still use it.
        scheduler.Finish();
        AmdFfx::Scope ffx_scope{"destroy(FSR 4)"};
        AmdFfx::Api().destroy_context(&context->context, nullptr);
    }
    context.reset();
}

bool AmdFsr4Upscaler::Prepare(VkExtent2D max_render_size, VkExtent2D upscale_size, bool hdr) {
    if (failed) {
        return false;
    }
    if (staging && staging_done.load(std::memory_order_acquire)) {
        staging_thread.join();
        if (staging->context == nullptr) {
            LOG_ERROR(Render_Vulkan, "AMD FSR 4: context creation failed ({})", staging->error);
            failed = true;
            staging.reset();
            return false;
        }
        Destroy(current);
        current = std::move(staging);
        LOG_INFO(Render_Vulkan, "AMD FSR 4: {} ready for {}x{} -> {}x{} ({})", current->provider,
                 current->max_render.width, current->max_render.height, current->upscale.width,
                 current->upscale.height, current->hdr ? "HDR" : "LDR");
    }
    const auto fits = [&](const Context& context) {
        return context.max_render.width >= max_render_size.width &&
               context.max_render.height >= max_render_size.height &&
               context.upscale.width == upscale_size.width &&
               context.upscale.height == upscale_size.height && context.hdr == hdr;
    };
    if (current && fits(*current)) {
        if (output_extent.width != upscale_size.width ||
            output_extent.height != upscale_size.height) {
            if (output) {
                scheduler.Finish();
            }
            output = memory_allocator.CreateImage(VkImageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = OUTPUT_FORMAT,
                .extent = {upscale_size.width, upscale_size.height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            });
            output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = *output,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = OUTPUT_FORMAT,
                .subresourceRange = COLOR_RANGE,
            });
            if (!exposure) {
                exposure = memory_allocator.CreateImage(VkImageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D,
                    .format = EXPOSURE_FORMAT,
                    .extent = {1, 1, 1},
                    .mipLevels = 1,
                    .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT,
                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                });
            }
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([image = *output, exp = *exposure](vk::CommandBuffer cmdbuf) {
                const std::array barriers{
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image,
                        .subresourceRange = COLOR_RANGE,
                    },
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = exp,
                        .subresourceRange = COLOR_RANGE,
                    },
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                           VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, {}, {}, barriers);
            });
            output_extent = upscale_size;
        }
        return true;
    }
    if (staging) {
        return false; // still being made
    }
    auto context = std::make_unique<Context>();
    context->max_render = max_render_size;
    context->upscale = upscale_size;
    context->hdr = hdr;
    staging = std::move(context);
    staging_done.store(false, std::memory_order_relaxed);
    staging_thread = std::jthread([this, device_context = DeviceContext(device)]() mutable {
        Context& c = *staging;
        const auto versions =
            AmdFfx::QueryVersions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, device_context);
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext = device_context;
        ffxCreateContextDescUpscaleVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.version = FFX_UPSCALER_VERSION;
        ffxOverrideVersion override_version{};
        override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        // Depth is [0,1] and finite (not inverted); the exposure comes in a 1x1 image.
        desc.flags = c.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0U;
        desc.maxRenderSize = {c.max_render.width, c.max_render.height};
        desc.maxUpscaleSize = {c.upscale.width, c.upscale.height};
        desc.fpMessage = AmdFfx::Message;
        desc.header.pNext = &backend.header;
        backend.header.pNext = &version.header;
        if (const auto id = VersionOverride(versions)) {
            override_version.versionId = *id;
            version.header.pNext = &override_version.header;
        }
        AmdFfx::Scope ffx_scope{"ffxCreateContext(FSR 4)"};
        if (const ffxReturnCode_t result =
                AmdFfx::Api().create_context(&c.context, &desc.header, nullptr);
            result != FFX_API_RETURN_OK) {
            c.context = nullptr;
            c.error = "ffxCreateContext returned " + std::to_string(result);
        } else {
            c.provider = AmdFfx::ProviderName(c.context);
        }
        staging_done.store(true, std::memory_order_release);
    });
    LOG_INFO(Render_Vulkan, "AMD FSR 4: creating a {}x{} -> {}x{} context in the background",
             max_render_size.width, max_render_size.height, upscale_size.width,
             upscale_size.height);
    return false;
}

VkImage AmdFsr4Upscaler::Stage(Staged& staged, const Image& source, VkExtent2D size,
                               VkImageAspectFlags aspect) {
    if (!staged.image || staged.format != source.format || staged.extent.width != size.width ||
        staged.extent.height != size.height) {
        if (staged.image) {
            scheduler.Finish();
        }
        const bool depth = aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
        staged.image = memory_allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = source.format,
            .extent = {size.width, size.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0U),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        });
        staged.format = source.format;
        staged.extent = size;
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([image = *staged.image, aspect](vk::CommandBuffer cmdbuf) {
            const VkImageMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{aspect, 0, 1, 0, 1},
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, barrier);
        });
    }
    return *staged.image;
}

bool AmdFsr4Upscaler::Upscale(const Inputs& in) {
    if (!current || failed) {
        return false;
    }
    while (ticks.size() >= MAX_DISPATCHES_IN_FLIGHT) {
        scheduler.Wait(ticks.front());
        ticks.pop_front();
    }
    while (!ticks.empty() && scheduler.IsFree(ticks.front())) {
        ticks.pop_front();
    }
    const auto plain = [&](const Image& image) {
        return image.layer == 0 && image.extent.width == in.render_size.width &&
               image.extent.height == in.render_size.height;
    };
    struct Copy {
        VkImage source;
        VkImage destination;
        u32 layer;
        VkImageAspectFlags aspect;
    };
    std::array<std::optional<Copy>, 2> copies;
    VkImage color = in.color.image;
    VkImage depth = in.depth.image;
    if (!plain(in.color)) {
        color = Stage(staged_color, in.color, in.render_size, VK_IMAGE_ASPECT_COLOR_BIT);
        copies[0] = Copy{in.color.image, color, in.color.layer, VK_IMAGE_ASPECT_COLOR_BIT};
    }
    if (!plain(in.depth)) {
        depth = Stage(staged_depth, in.depth, in.render_size, VK_IMAGE_ASPECT_DEPTH_BIT);
        copies[1] = Copy{in.depth.image, depth, in.depth.layer, VK_IMAGE_ASPECT_DEPTH_BIT};
    }

    constexpr u32 read = FFX_API_RESOURCE_STATE_GENERIC_READ; // GENERAL layout
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    const u32 rw = in.render_size.width;
    const u32 rh = in.render_size.height;
    d.color = AmdFfx::Image2D(color, in.color.format, rw, rh, false, read);
    d.depth = AmdFfx::Image2D(depth, in.depth.format, rw, rh, false, read);
    d.motionVectors = AmdFfx::Image2D(in.motion_vectors.image, in.motion_vectors.format,
                                      in.motion_vectors.extent.width,
                                      in.motion_vectors.extent.height, false, read);
    d.exposure = AmdFfx::Image2D(*exposure, EXPOSURE_FORMAT, 1, 1, false, read);
    if (in.reactive) {
        d.reactive = AmdFfx::Image2D(in.reactive->image, in.reactive->format,
                                     in.reactive->extent.width, in.reactive->extent.height, false,
                                     read);
    }
    d.output = AmdFfx::Image2D(*output, OUTPUT_FORMAT, in.upscale_size.width,
                               in.upscale_size.height, true,
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {in.jitter[0], in.jitter[1]};
    d.motionVectorScale = {in.motion_vector_scale[0], in.motion_vector_scale[1]};
    d.renderSize = {rw, rh};
    d.upscaleSize = {in.upscale_size.width, in.upscale_size.height};
    d.enableSharpening = in.sharpen;
    d.sharpness = in.sharpness;
    d.frameTimeDelta = in.frame_time_ms;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.camera_near;
    d.cameraFar = in.camera_far;
    d.cameraFovAngleVertical = in.vertical_fov;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, d, copies, context = &current->context, exp = *exposure,
                      exposure_value = in.exposure.value_or(1.0f)](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier to_transfer{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, to_transfer);
        for (const auto& copy : copies) {
            if (!copy) {
                continue;
            }
            const VkImageCopy region{
                .srcSubresource{copy->aspect, 0, copy->layer, 1},
                .srcOffset{},
                .dstSubresource{copy->aspect, 0, 0, 1},
                .dstOffset{},
                .extent{d.renderSize.width, d.renderSize.height, 1},
            };
            cmdbuf.CopyImage(copy->source, VK_IMAGE_LAYOUT_GENERAL, copy->destination,
                             VK_IMAGE_LAYOUT_GENERAL, region);
        }
        VkClearColorValue value{};
        value.float32[0] = exposure_value;
        cmdbuf.ClearColorImage(exp, VK_IMAGE_LAYOUT_GENERAL, value, COLOR_RANGE);
        const VkMemoryBarrier to_compute{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, to_compute);
        ffxDispatchDescUpscale dispatch = d;
        dispatch.commandList = *cmdbuf;
        AmdFfx::Scope ffx_scope{"ffxDispatch(FSR 4 upscale)"};
        if (const ffxReturnCode_t result = AmdFfx::Api().dispatch(context, &dispatch.header);
            result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "AMD FSR 4: ffxDispatch returned {}", result);
            failed = true;
        }
        const VkMemoryBarrier done{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, done);
    });
    // AMD's backend bound its own pipelines and descriptor sets.
    scheduler.InvalidateState();
    ticks.push_back(scheduler.CurrentTick());
    return true;
}

} // namespace Vulkan
