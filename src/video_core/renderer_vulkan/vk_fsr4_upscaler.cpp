// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4 through an FFX API upscaler library loaded at run time. The library (for example
// github.com/Astyyyyy/fsr4-vk's amd_fidelityfx_upscaler_vk, built from AMD's SDK 2.3.0 model)
// takes plain Vulkan images: the inputs in SHADER_READ_ONLY_OPTIMAL (FFX "compute read"), the
// output in GENERAL ("unordered access"), and records into our command buffer.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <boost/container/static_vector.hpp>

// The vendored FFX API headers declare their entry points with MSVC's __declspec; this file only
// calls them through pointers.
#ifndef _WIN32
#define __declspec(x)
#endif
#include "upscalers/include/ffx_upscale.h"
#ifndef _WIN32
#undef __declspec
#endif

#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
#ifdef _WIN32
constexpr const char* LIBRARY_NAME = "amd_fidelityfx_upscaler_vk.dll";
#else
constexpr const char* LIBRARY_NAME = "amd_fidelityfx_upscaler_vk.so";
#endif
constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
// ffx_api_vk.h's FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK (not in the vendored headers).
constexpr ffxStructType_t BACKEND_VK_DESC_TYPE = 0x0000003u;
struct CreateBackendVkDesc {
    ffxCreateContextDescHeader header;
    VkDevice device;
    VkPhysicalDevice physical_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
};

constexpr VkImageSubresourceRange COLOR_RANGE{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

u32 SurfaceFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R16_FLOAT;
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT:
        return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    default:
        return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

FfxApiResource Resource(VkImage image, VkFormat format, VkExtent2D size, u32 usage, u32 state) {
    FfxApiResource resource{};
    resource.resource = reinterpret_cast<void*>(image);
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = SurfaceFormat(format);
    resource.description.width = size.width;
    resource.description.height = size.height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.flags = 0;
    resource.description.usage = usage;
    resource.state = state;
    return resource;
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageSubresourceRange range, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
}

VkImageMemoryBarrier Barrier(const Fsr4Upscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return Barrier(image.image, range, src_access, dst_access, old_layout, new_layout);
}

void Message(uint32_t type, const wchar_t* message) {
    std::string text;
    for (const wchar_t* c = message; c && *c; ++c) {
        text.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "FSR 4: {}", text);
    } else {
        LOG_INFO(Render_Vulkan, "FSR 4: {}", text);
    }
}
} // Anonymous namespace

struct Fsr4Upscaler::Api {
    PfnFfxCreateContext create_context{};
    PfnFfxDestroyContext destroy_context{};
    PfnFfxDispatch dispatch{};
};

std::shared_ptr<Common::DynamicLibrary> Fsr4Upscaler::OpenLibrary() {
    static const std::shared_ptr<Common::DynamicLibrary> library =
        []() -> std::shared_ptr<Common::DynamicLibrary> {
        // EDEN_NX2_FSR4_LIB, else the library's own name in the user folder.
        std::string path;
        if (const char* const env = std::getenv("EDEN_NX2_FSR4_LIB"); env && env[0] != '\0') {
            path = env;
        } else {
            const auto file = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / LIBRARY_NAME;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec)) {
                return nullptr;
            }
            path = Common::FS::PathToUTF8String(file);
        }
        auto lib = std::make_shared<Common::DynamicLibrary>(path.c_str());
        if (!lib->IsOpen()) {
            LOG_WARNING(Render_Vulkan, "FSR 4: {} doesn't load; using FSR 3.1", path);
            return nullptr;
        }
        LOG_INFO(Render_Vulkan, "FSR 4: upscaler library {}", path);
        return lib;
    }();
    return library;
}

Fsr4Upscaler::Fsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      library{OpenLibrary()}, api{std::make_unique<Api>()} {
    if (!library || !library->GetSymbol("ffxCreateContext", &api->create_context) ||
        !library->GetSymbol("ffxDestroyContext", &api->destroy_context) ||
        !library->GetSymbol("ffxDispatch", &api->dispatch)) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    CreateExposure();
}

Fsr4Upscaler::~Fsr4Upscaler() {
    DestroyContext();
}

void Fsr4Upscaler::DestroyContext() {
    if (context == nullptr) {
        return;
    }
    scheduler.Finish(); // recorded dispatches may still use it
    ffxContext ctx = context;
    api->destroy_context(&ctx, nullptr);
    context = nullptr;
}

bool Fsr4Upscaler::CreateContext(VkExtent2D render_size, VkExtent2D upscale_size) {
    DestroyContext();
    CreateBackendVkDesc backend{};
    backend.header.type = BACKEND_VK_DESC_TYPE;
    backend.header.pNext = nullptr;
    backend.device = *device.GetLogical();
    backend.physical_device = device.GetPhysical();
    backend.get_device_proc_addr = device.GetDispatchLoader().vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &backend.header;
    // Tonemapped LDR colour, standard (not reversed) finite depth, exposure given (unit).
    create.flags = 0;
    create.maxRenderSize = {render_size.width, render_size.height};
    create.maxUpscaleSize = {upscale_size.width, upscale_size.height};
    create.fpMessage = Message;
    ffxContext ctx{};
    const ffxReturnCode_t result = api->create_context(&ctx, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "FSR 4: ffxCreateContext {}x{} -> {}x{} failed ({}); using FSR 3.1",
                  render_size.width, render_size.height, upscale_size.width, upscale_size.height,
                  result);
        return false;
    }
    context = ctx;
    render_extent = render_size;
    upscale_extent = upscale_size;
    LOG_INFO(Render_Vulkan, "FSR 4: context {}x{} -> {}x{}", render_size.width, render_size.height,
             upscale_size.width, upscale_size.height);
    return true;
}

void Fsr4Upscaler::CreateExposure() {
    exposure_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *exposure_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0,
                               Barrier(image, COLOR_RANGE, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
        VkClearColorValue one{};
        one.float32[0] = 1.0f;
        cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, one, COLOR_RANGE);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               Barrier(image, COLOR_RANGE, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_ACCESS_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    });
}

void Fsr4Upscaler::CreateOutput(VkExtent2D upscale_size) {
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
        .subresourceRange = COLOR_RANGE,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               Barrier(image, COLOR_RANGE, 0,
                                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
    });
}

bool Fsr4Upscaler::Upscale(const Inputs& in) {
    if (failed.load()) {
        return false;
    }
    if (context == nullptr || in.render_size.width != render_extent.width ||
        in.render_size.height != render_extent.height ||
        in.upscale_size.width != upscale_extent.width ||
        in.upscale_size.height != upscale_extent.height) {
        if (!CreateContext(in.render_size, in.upscale_size)) {
            failed = true;
            return false;
        }
        CreateOutput(in.upscale_size);
    }

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.header.pNext = nullptr;
    dispatch.color = Resource(in.color.image, in.color.format, in.render_size, 0,
                              FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = Resource(in.depth.image, in.depth.format, in.render_size,
                              FFX_API_RESOURCE_USAGE_DEPTHTARGET,
                              FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = Resource(in.motion_vectors.image, in.motion_vectors.format,
                                      in.render_size, 0, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.exposure = Resource(*exposure_image, VK_FORMAT_R32_SFLOAT, {1, 1}, 0,
                                 FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (in.reactive) {
        dispatch.reactive = Resource(in.reactive->image, in.reactive->format, in.render_size, 0,
                                     FFX_API_RESOURCE_STATE_COMPUTE_READ);
    }
    dispatch.output = Resource(*output_image, OUTPUT_FORMAT, in.upscale_size,
                               FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {in.jitter[0], in.jitter[1]};
    dispatch.motionVectorScale = {1.0f, 1.0f};
    dispatch.renderSize = {in.render_size.width, in.render_size.height};
    dispatch.upscaleSize = {in.upscale_size.width, in.upscale_size.height};
    dispatch.enableSharpening = in.sharpen;
    dispatch.sharpness = in.sharpness;
    dispatch.frameTimeDelta = in.frame_time_ms;
    dispatch.preExposure = 1.0f;
    dispatch.reset = in.reset;
    dispatch.cameraNear = in.camera_near;
    dispatch.cameraFar = in.camera_far;
    dispatch.cameraFovAngleVertical = in.vertical_fov;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = 0;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, dispatch,
                      output = *output_image](vk::CommandBuffer cmdbuf) {
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs stay in GENERAL otherwise; FSR 4 reads them as sampled images.
        boost::container::static_vector<VkImageMemoryBarrier, 5> to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(output, COLOR_RANGE, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.reactive) {
            to_read.push_back(Barrier(*in.reactive, WRITES, VK_ACCESS_SHADER_READ_BIT,
                                      VK_IMAGE_LAYOUT_GENERAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        ffxDispatchDescUpscale recorded = dispatch;
        recorded.commandList = static_cast<void*>(*cmdbuf);
        ffxContext ctx = context;
        const ffxReturnCode_t result = api->dispatch(&ctx, &recorded.header);
        if (result != FFX_API_RETURN_OK && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "FSR 4: ffxDispatch failed ({}); using FSR 3.1", result);
        }

        boost::container::static_vector<VkImageMemoryBarrier, 5> back{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT, 0,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(output, COLOR_RANGE, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        };
        if (in.reactive) {
            back.push_back(Barrier(*in.reactive, VK_ACCESS_SHADER_READ_BIT, 0,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_IMAGE_LAYOUT_GENERAL));
        }
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, back);
    });
    return true;
}

} // namespace Vulkan
