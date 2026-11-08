// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include <windows.h>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/present/amd_frame_gen.h"
#include "video_core/renderer_vulkan/present/perf_overlay_pass.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/amd_ffx.h"
#include "video_core/vulkan_common/amd_ffx_api.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

namespace {

std::atomic<AmdFrameGen*> g_active{};

/// AMD's swapchain submits to the game queue from its own threads; Lichii's threads submit to
/// it under the scheduler's submit mutex. VkQueueInfoFFXAPI::submitFunc has no queue argument,
/// so the one game queue lives here.
struct GameQueue {
    VkQueue queue{};
    PFN_vkQueueSubmit submit{};
    std::mutex* mutex{};
} g_game_queue;

VkResult SubmitToGameQueue(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    AmdFfx::Scope ffx_scope{"submitFunc (game queue)"};
    std::scoped_lock lock{*g_game_queue.mutex};
    return g_game_queue.submit(g_game_queue.queue, count, submits, fence);
}

VkDeviceContext DeviceContext(const Device& device) {
    return VkDeviceContext{
        .vkDevice = *device.GetLogical(),
        .vkPhysicalDevice = device.GetPhysical(),
        .vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr,
    };
}

/// Adrenalin's version from driverInfo ("26.10.2 (LLPC)"), as year * 10000 + month * 100 + minor.
u32 AdrenalinVersion(std::string_view info) {
    std::array<u32, 3> parts{};
    const char* it = info.data();
    const char* const end = info.data() + info.size();
    for (u32& part : parts) {
        const auto result = std::from_chars(it, end, part);
        if (result.ec != std::errc{}) {
            return 0;
        }
        it = result.ptr;
        if (it == end || *it != '.') {
            break;
        }
        ++it;
    }
    return parts[0] * 10000 + parts[1] * 100 + parts[2];
}

bool HagsEnabled() {
    DWORD value{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                        L"HwSchMode", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value == 2;
}

} // Anonymous namespace

struct AmdFrameGen::Impl {
    const Device& device;
    Scheduler& scheduler;
    ffxContext swapchain_context{};
    ffxContext frame_gen_context{};
    ffxQueryDescSwapchainReplacementFunctionsVK functions{};
    VkSwapchainKHR swapchain{};
    VkExtent2D display_size{};
    VkFormat format{};
    bool hdr{};
    u64 frame_id{};
    std::optional<u64> configured;
    FfxApiRect2D generation_rect{};
    bool covers_monitor{};
    bool vsync_off{};
    bool generating{};
    bool logged_generation{};
    std::optional<std::chrono::steady_clock::time_point> last_prepare;
    // The UI resource (ComposeUi): premultiplied alpha at the swapchain's size and view format.
    vk::Image ui_image;
    vk::ImageView ui_view;
    vk::RenderPass ui_render_pass;
    vk::Framebuffer ui_framebuffer;
    VkFormat ui_format{};
    VkExtent2D ui_extent{};
    bool ui_fresh{};
    bool ui_registered{};
    // HUD-less back buffers (ComposeHudless), one per frame in flight: frame generation reads
    // frame N's while frame N+1 renders.
    std::array<vk::Image, 3> hudless;
    VkExtent2D hudless_extent{};
    VkFormat hudless_format{};
    vk::Image hudless_encode; ///< The scene sRGB-encoded at its size (a blit into an sRGB format)
    VkExtent2D encode_extent{};
    VkFormat encode_format{};
    bool hudless_ready{};

    [[nodiscard]] VkImage HudlessFor(u64 frame) const {
        return *hudless[frame % hudless.size()];
    }

    void RegisterUi(VkImage image) {
        ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceVK ui{};
        ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FGSWAPCHAIN_REGISTERUIRESOURCE_VK;
        if (image != VK_NULL_HANDLE) {
            ui.uiResource = AmdFfx::Image2D(image, ui_format, ui_extent.width, ui_extent.height,
                                            false, FFX_API_RESOURCE_STATE_GENERIC_READ);
            // The swapchain copies the UI at present: the next frame may draw it again.
            ui.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                       FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
        }
        AmdFfx::Scope ffx_scope{"ffxConfigure(UI resource)"};
        if (const auto result = AmdFfx::Api().configure(&swapchain_context, &ui.header);
            result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "AMD FSR frame generation: UI resource configure returned {}",
                      result);
        }
        ui_registered = image != VK_NULL_HANDLE;
    }

    void CreateUiImage(MemoryAllocator& allocator, VkFormat format, VkExtent2D extent) {
        ui_framebuffer.reset();
        ui_render_pass.reset();
        ui_view.reset();
        ui_image.reset();
        ui_format = format;
        ui_extent = extent;
        ui_image = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
        ui_view = CreateWrappedImageView(device, ui_image, format);
        // Compatible with the overlay pipeline's render pass (same format and samples); it loads
        // what the clear wrote.
        ui_render_pass = CreateWrappedRenderPass(device, format, VK_IMAGE_LAYOUT_GENERAL);
        ui_framebuffer = CreateWrappedFramebuffer(device, ui_render_pass, ui_view, extent);
        ui_fresh = true;
    }

    explicit Impl(const Device& device_, Scheduler& scheduler_)
        : device{device_}, scheduler{scheduler_} {}

    void Configure(bool enabled) {
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = swapchain;
        config.frameGenerationCallback = &Generate;
        config.frameGenerationCallbackUserContext = this;
        config.frameGenerationEnabled = enabled;
        config.allowAsyncWorkloads = true;
        config.generationRect = generation_rect;
        config.frameID = frame_id;
        if (enabled && hudless_ready) {
            config.HUDLessColor =
                AmdFfx::Image2D(HudlessFor(frame_id), hudless_format, hudless_extent.width,
                                hudless_extent.height, false, FFX_API_RESOURCE_STATE_GENERIC_READ);
        }
        AmdFfx::Scope ffx_scope{"ffxConfigure(frame generation)"};
        if (const auto result = AmdFfx::Api().configure(&frame_gen_context, &config.header);
            result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "AMD FSR frame generation: ffxConfigure returned {}", result);
        }
        configured = frame_id;
        generating = enabled;
    }

    /// AMD's swapchain calls this inside present to record frame generation.
    static ffxReturnCode_t Generate(ffxDispatchDescFrameGeneration* params, void* context) {
        AmdFfx::Scope ffx_scope{"frameGenerationCallback"};
        auto* const self = static_cast<Impl*>(context);
        params->backbufferTransferFunction = self->hdr ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
                                                       : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        if (self->hdr) {
            params->minMaxLuminance[0] = 0.0f;
            params->minMaxLuminance[1] = 10000.0f;
        }
        if (!std::exchange(self->logged_generation, true)) {
            LOG_INFO(Render_Vulkan,
                     "AMD FSR frame generation: AMD's swapchain asks for {} generated frame(s) per "
                     "real frame",
                     params->numGeneratedFrames);
        }
        return AmdFfx::Api().dispatch(&self->frame_gen_context, &params->header);
    }

    void DestroyFrameGenContext() {
        if (frame_gen_context == nullptr) {
            return;
        }
        AmdFfx::Scope ffx_scope{"WaitForPresents + destroy(frame generation)"};
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
        wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
        AmdFfx::Api().dispatch(&swapchain_context, &wait.header);
        scheduler.Finish();
        AmdFfx::Api().destroy_context(&frame_gen_context, nullptr);
        frame_gen_context = nullptr;
    }

    bool CreateFrameGenContext() {
        VkDeviceContext device_context = DeviceContext(device);
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.deviceContext = device_context;
        ffxCreateContextDescFrameGenerationVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        version.version = FFX_FRAMEGENERATION_VERSION;
        ffxCreateContextDescFrameGeneration desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        // Ocarina's depth is [0,1], finite and not inverted; the motion is at the render size.
        desc.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT |
                     (hdr ? FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE : 0U);
        desc.displaySize = {display_size.width, display_size.height};
        // The 3D is at most the display or the game's 4K output at a 1.5x scale.
        desc.maxRenderSize = {(std::max)(display_size.width, 3840U),
                              (std::max)(display_size.height, 2160U)};
        desc.backBufferFormat = ffxApiGetSurfaceFormatVK(format);
        desc.header.pNext = &backend.header;
        backend.header.pNext = &version.header;
        AmdFfx::Scope ffx_scope{"ffxCreateContext(frame generation)"};
        if (const auto result =
                AmdFfx::Api().create_context(&frame_gen_context, &desc.header, nullptr);
            result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "AMD FSR frame generation: ffxCreateContext returned {}",
                      result);
            frame_gen_context = nullptr;
            return false;
        }
        LOG_INFO(Render_Vulkan, "AMD FSR frame generation: {} for {}x{} {}",
                 AmdFfx::ProviderName(frame_gen_context), display_size.width,
                 display_size.height, hdr ? "HDR10" : "SDR");
        configured.reset();
        return true;
    }
};

AmdFrameGen::AmdFrameGen(const Device& device, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(device, scheduler)} {
    g_game_queue = {
        .queue = *device.GetGraphicsQueue(),
        .submit = device.GetDispatchLoader().vkQueueSubmit,
        .mutex = &scheduler.submit_mutex,
    };
}

AmdFrameGen::~AmdFrameGen() {
    AmdFrameGen* self = this;
    g_active.compare_exchange_strong(self, nullptr);
    auto& state = *impl;
    if (state.swapchain_context == nullptr) {
        return;
    }
    state.DestroyFrameGenContext();
    AmdFfx::Scope ffx_scope{"destroy(frame generation swapchain)"};
    AmdFfx::Api().destroy_context(&state.swapchain_context, nullptr);
    // After the context, as the header asks; pContext is the frame generation swapchain.
    if (state.functions.pOutDestroySwapchainFFXAPI != nullptr && state.swapchain != nullptr) {
        state.functions.pOutDestroySwapchainFFXAPI(*state.device.GetLogical(), state.swapchain,
                                                   nullptr, state.swapchain);
    }
}

bool AmdFrameGen::Wanted() {
    return Settings::values.frame_gen.GetValue() &&
           Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::AmdFsr;
}

bool AmdFrameGen::IsAvailable(const Device& device) {
    static const bool available = [&device] {
        if (!AmdFfx::HasFrameGeneration() || !device.HasFrameGenQueues()) {
            return false;
        }
        VkDeviceContext context = DeviceContext(device);
        std::string list;
        for (const auto& version :
             AmdFfx::QueryVersions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK, context)) {
            list += (list.empty() ? "" : ", ") + version.name;
        }
        std::string fg;
        for (const auto& version :
             AmdFfx::QueryVersions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, context)) {
            fg += (fg.empty() ? "" : ", ") + version.name;
        }
        LOG_INFO(Render_Vulkan, "AMD FSR frame generation versions: swapchain {}; generation {}",
                 list.empty() ? "none" : list, fg.empty() ? "none" : fg);
        return !list.empty() && !fg.empty();
    }();
    return available;
}

AmdFrameGen* AmdFrameGen::Active() {
    return g_active.load(std::memory_order_acquire);
}

VkResult AmdFrameGen::CreateSwapchain(const VkSwapchainCreateInfoKHR& ci,
                                      VkSwapchainKHR& swapchain) {
    auto& state = *impl;
    const bool resized = state.display_size.width != ci.imageExtent.width ||
                         state.display_size.height != ci.imageExtent.height ||
                         state.format != ci.imageFormat;
    state.display_size = ci.imageExtent;
    state.format = ci.imageFormat;
    if (state.generation_rect.width == 0 || resized) {
        // The whole image until the renderer sets the game's area (SetGenerationRect).
        state.generation_rect = {0, 0, static_cast<s32>(ci.imageExtent.width),
                                 static_cast<s32>(ci.imageExtent.height)};
    }
    state.hdr = ci.imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
    if (state.swapchain_context != nullptr) {
        // In place: the frame generation swapchain stays (ffx_api_framegeneration_vk.h:129).
        if (resized) {
            state.DestroyFrameGenContext();
        }
        VkSwapchainCreateInfoKHR info = ci;
        info.oldSwapchain = VK_NULL_HANDLE;
        AmdFfx::Scope ffx_scope{"pOutCreateSwapchainFFXAPI"};
        const VkResult result = state.functions.pOutCreateSwapchainFFXAPI(
            *state.device.GetLogical(), &info, nullptr, &state.swapchain, state.swapchain);
        if (result != VK_SUCCESS) {
            return result;
        }
        swapchain = state.swapchain;
        if (state.frame_gen_context == nullptr && !state.CreateFrameGenContext()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        g_active.store(this, std::memory_order_release);
        return VK_SUCCESS;
    }
    const auto& device = state.device;
    const auto queue = [](const Device::FrameGenQueue& queue) {
        return VkQueueInfoFFXAPI{.queue = *queue.queue, .familyIndex = queue.family,
                                 .submitFunc = nullptr};
    };
    ffxCreateContextDescFrameGenerationSwapChainVersionVK version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainVK desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    desc.physicalDevice = device.GetPhysical();
    desc.device = *device.GetLogical();
    state.swapchain = VK_NULL_HANDLE;
    desc.swapchain = &state.swapchain;
    desc.allocator = nullptr;
    desc.createInfo = ci;
    desc.createInfo.oldSwapchain = VK_NULL_HANDLE;
    desc.gameQueue = {.queue = *device.GetGraphicsQueue(),
                      .familyIndex = device.GetGraphicsFamily(),
                      .submitFunc = &SubmitToGameQueue};
    desc.asyncComputeQueue = queue(device.GetFrameGenComputeQueue());
    desc.presentQueue = queue(device.GetFrameGenPresentQueue());
    desc.imageAcquireQueue = queue(device.GetFrameGenAcquireQueue());
    desc.header.pNext = &version.header;
    AmdFfx::Scope ffx_scope{"ffxCreateContext(frame generation swapchain)"};
    if (const auto result =
            AmdFfx::Api().create_context(&state.swapchain_context, &desc.header, nullptr);
        result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "AMD FSR frame generation swapchain: ffxCreateContext returned {}",
                  result);
        state.swapchain_context = nullptr;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    state.functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    if (AmdFfx::Api().query(&state.swapchain_context, &state.functions.header) !=
            FFX_API_RETURN_OK ||
        state.functions.pOutQueuePresentKHR == nullptr) {
        LOG_ERROR(Render_Vulkan, "AMD FSR frame generation swapchain: no replacement functions");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LOG_INFO(Render_Vulkan, "AMD FSR frame generation swapchain: {} ({}x{}, {} images)",
             AmdFfx::ProviderName(state.swapchain_context), ci.imageExtent.width,
             ci.imageExtent.height, ci.minImageCount);
    swapchain = state.swapchain;
    if (!state.CreateFrameGenContext()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    g_active.store(this, std::memory_order_release);
    return VK_SUCCESS;
}

std::vector<VkImage> AmdFrameGen::GetImages(VkSwapchainKHR swapchain) const {
    const auto& state = *impl;
    u32 count = 0;
    AmdFfx::Scope ffx_scope{"pOutGetSwapchainImagesKHR"};
    state.functions.pOutGetSwapchainImagesKHR(*state.device.GetLogical(), swapchain, &count,
                                              nullptr);
    std::vector<VkImage> images(count);
    state.functions.pOutGetSwapchainImagesKHR(*state.device.GetLogical(), swapchain, &count,
                                              images.data());
    images.resize(count);
    return images;
}

VkResult AmdFrameGen::AcquireNextImage(VkSwapchainKHR swapchain, u64 timeout,
                                       VkSemaphore semaphore, u32* image_index) const {
    AmdFfx::Scope ffx_scope{"pOutAcquireNextImageKHR"};
    return impl->functions.pOutAcquireNextImageKHR(*impl->device.GetLogical(), swapchain, timeout,
                                                   semaphore, VK_NULL_HANDLE, image_index);
}

void AmdFrameGen::ComposeUi(MemoryAllocator& allocator, VkFormat format,
                            const PerfOverlayDraw* draw) {
    auto& state = *impl;
    if (state.swapchain_context == nullptr) {
        return;
    }
    if (draw == nullptr) {
        if (state.ui_registered) {
            state.RegisterUi(VK_NULL_HANDLE);
        }
        return;
    }
    if (!state.ui_image || state.ui_format != format ||
        state.ui_extent.width != state.display_size.width ||
        state.ui_extent.height != state.display_size.height) {
        // The swapchain may still copy the old image for a queued present.
        WaitForPresents();
        state.scheduler.Finish();
        state.CreateUiImage(allocator, format, state.display_size);
    }
    state.scheduler.RequestOutsideRenderPassOperationContext();
    state.scheduler.Record([image = *state.ui_image, render_pass = *state.ui_render_pass,
                            framebuffer = *state.ui_framebuffer, extent = state.ui_extent,
                            fresh = std::exchange(state.ui_fresh, false),
                            draw = *draw](vk::CommandBuffer cmdbuf) {
        constexpr VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        // After the swapchain's copy of the last frame's UI (same queue), before the clear.
        const VkImageMemoryBarrier to_clear{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, to_clear);
        cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{},
                               std::array{range});
        const VkMemoryBarrier to_draw{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, to_draw);
        const VkRenderPassBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = render_pass,
            .framebuffer = framebuffer,
            .renderArea = {.offset = {0, 0}, .extent = extent},
            .clearValueCount = 0,
            .pClearValues = nullptr,
        };
        cmdbuf.BeginRenderPass(begin, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<f32>(extent.width),
            .height = static_cast<f32>(extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        cmdbuf.SetViewport(0, viewport);
        cmdbuf.SetScissor(0, VkRect2D{.offset = {0, 0}, .extent = extent});
        RecordPerfOverlay(cmdbuf, draw);
        cmdbuf.EndRenderPass();
        const VkMemoryBarrier to_compose{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, to_compose);
    });
    state.scheduler.InvalidateState();
    state.RegisterUi(*state.ui_image);
}

namespace {

/// The sRGB format with the same bytes per texel and channel order, if any.
VkFormat SrgbVariant(VkFormat format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

vk::Image CreateTransferImage(MemoryAllocator& allocator, VkFormat format, VkExtent2D extent,
                              VkImageUsageFlags usage) {
    return allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
}

constexpr VkImageSubresourceLayers ColorLayer{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

} // Anonymous namespace

void AmdFrameGen::ComposeHudless(MemoryAllocator& allocator, VkImage frame_image,
                                 VkFormat frame_format, VkExtent2D frame_extent, VkImage scene,
                                 VkExtent2D scene_extent, bool scene_fresh, VkRect2D rect) {
    auto& state = *impl;
    // The frame is copied to the back buffer as it is only when they match (CopyToSwapchain).
    if (state.frame_gen_context == nullptr || frame_format != state.format ||
        frame_extent.width != state.display_size.width ||
        frame_extent.height != state.display_size.height) {
        state.hudless_ready = false;
        return;
    }
    std::vector<VkImage> fresh;
    if (!state.hudless[0] || state.hudless_format != frame_format ||
        state.hudless_extent.width != frame_extent.width ||
        state.hudless_extent.height != frame_extent.height) {
        // Frame generation may still read the old ones for a queued present.
        WaitForPresents();
        state.scheduler.Finish();
        state.hudless_format = frame_format;
        state.hudless_extent = frame_extent;
        for (auto& image : state.hudless) {
            image = CreateTransferImage(allocator, frame_format, frame_extent,
                                        VK_IMAGE_USAGE_SAMPLED_BIT);
            fresh.push_back(*image);
        }
    }
    const VkFormat encode_format = SrgbVariant(frame_format);
    const bool one_to_one =
        scene != VK_NULL_HANDLE && scene_fresh && encode_format != VK_FORMAT_UNDEFINED &&
        rect.extent.width == scene_extent.width && rect.extent.height == scene_extent.height &&
        rect.offset.x >= 0 && rect.offset.y >= 0 &&
        static_cast<u32>(rect.offset.x) + rect.extent.width <= frame_extent.width &&
        static_cast<u32>(rect.offset.y) + rect.extent.height <= frame_extent.height;
    if (one_to_one && (!state.hudless_encode || state.encode_format != encode_format ||
                       state.encode_extent.width != scene_extent.width ||
                       state.encode_extent.height != scene_extent.height)) {
        state.scheduler.Finish();
        state.encode_format = encode_format;
        state.encode_extent = scene_extent;
        state.hudless_encode = CreateTransferImage(allocator, encode_format, scene_extent,
                                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        fresh.push_back(*state.hudless_encode);
    }
    state.scheduler.RequestOutsideRenderPassOperationContext();
    state.scheduler.Record([fresh = std::move(fresh), frame_image, frame_extent,
                            target = state.HudlessFor(state.frame_id), scene, scene_extent,
                            encode = one_to_one ? *state.hudless_encode : VK_NULL_HANDLE,
                            rect](vk::CommandBuffer cmdbuf) {
        for (const VkImage image : fresh) {
            const VkImageMemoryBarrier init{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, init);
        }
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, before);
        // The presented frame, HUD included...
        const VkImageCopy whole{
            .srcSubresource = ColorLayer,
            .srcOffset = {0, 0, 0},
            .dstSubresource = ColorLayer,
            .dstOffset = {0, 0, 0},
            .extent = {frame_extent.width, frame_extent.height, 1},
        };
        cmdbuf.CopyImage(frame_image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_GENERAL,
                         whole);
        if (encode != VK_NULL_HANDLE) {
            // ...with the scene before the HUD over the game's area, encoded as the guest's
            // plain copy to its sRGB framebuffer encodes it.
            const VkImageBlit blit{
                .srcSubresource = ColorLayer,
                .srcOffsets = {VkOffset3D{0, 0, 0},
                               VkOffset3D{static_cast<s32>(scene_extent.width),
                                          static_cast<s32>(scene_extent.height), 1}},
                .dstSubresource = ColorLayer,
                .dstOffsets = {VkOffset3D{0, 0, 0},
                               VkOffset3D{static_cast<s32>(scene_extent.width),
                                          static_cast<s32>(scene_extent.height), 1}},
            };
            cmdbuf.BlitImage(scene, VK_IMAGE_LAYOUT_GENERAL, encode, VK_IMAGE_LAYOUT_GENERAL, blit,
                             VK_FILTER_NEAREST);
            const VkMemoryBarrier encoded{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, encoded);
            const VkImageCopy area{
                .srcSubresource = ColorLayer,
                .srcOffset = {0, 0, 0},
                .dstSubresource = ColorLayer,
                .dstOffset = {rect.offset.x, rect.offset.y, 0},
                .extent = {rect.extent.width, rect.extent.height, 1},
            };
            cmdbuf.CopyImage(encode, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_GENERAL,
                             area);
        }
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               0, after);
    });
    state.hudless_ready = true;
}

void AmdFrameGen::WaitForPresents() {
    if (impl->swapchain_context == nullptr) {
        return;
    }
    AmdFfx::Scope ffx_scope{"WaitForPresents"};
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    AmdFfx::Api().dispatch(&impl->swapchain_context, &wait.header);
}

void AmdFrameGen::SetHdrMetadata(VkSwapchainKHR swapchain,
                                 const VkHdrMetadataEXT& metadata) const {
    if (impl->functions.pOutSetHdrMetadataEXT != nullptr) {
        AmdFfx::Scope ffx_scope{"pOutSetHdrMetadataEXT"};
        impl->functions.pOutSetHdrMetadataEXT(*impl->device.GetLogical(), 1, &swapchain,
                                              &metadata);
    }
}

VkResult AmdFrameGen::Present(VkQueue game_queue, const VkPresentInfoKHR& present_info) {
    auto& state = *impl;
    if (state.frame_gen_context != nullptr && state.configured != state.frame_id) {
        // No inputs this frame (a menu, a load, the game's own image): present it as rendered.
        state.Configure(false);
    }
    AmdFfx::Scope ffx_scope{"pOutQueuePresentKHR"};
    const VkResult result = state.functions.pOutQueuePresentKHR(game_queue, &present_info);
    ++state.frame_id;
    return result;
}

void AmdFrameGen::SetGenerationRect(s32 left, s32 top, s32 width, s32 height) {
    impl->generation_rect = {left, top, width, height};
}

void AmdFrameGen::SetPresentationState(bool covers_monitor, bool vsync_off) {
    impl->covers_monitor = covers_monitor;
    impl->vsync_off = vsync_off;
}

void AmdFrameGen::Prepare(Scheduler& scheduler, const PrepareInputs& in) {
    auto& state = *impl;
    if (state.frame_gen_context == nullptr || state.configured == state.frame_id || !in.camera) {
        return;
    }
    if (in.depth_layer != 0) {
        // FFX takes whole images: a depth in another array layer leaves the frame ungenerated.
        static bool logged{};
        if (!std::exchange(logged, true)) {
            LOG_WARNING(Render_Vulkan, "AMD FSR frame generation: scene depth in array layer {}",
                        in.depth_layer);
        }
        return;
    }
    // FG 4.0.2's mandatory order: configure, prepare, then generate in present.
    state.Configure(true);
    const auto now = std::chrono::steady_clock::now();
    const f32 frame_time_ms =
        state.last_prepare
            ? std::chrono::duration<f32, std::milli>(now - *state.last_prepare).count()
            : 1000.0f / 60.0f;
    state.last_prepare = now;

    constexpr u32 read = FFX_API_RESOURCE_STATE_GENERIC_READ; // GENERAL layout
    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = state.frame_id;
    prepare.flags = 0;
    prepare.renderSize = {in.render_size.width, in.render_size.height};
    prepare.jitterOffset = {in.jitter[0], in.jitter[1]};
    prepare.motionVectorScale = {in.motion_vector_scale[0], in.motion_vector_scale[1]};
    prepare.frameTimeDelta = frame_time_ms;
    prepare.reset = in.reset;
    prepare.cameraNear = in.camera_near;
    prepare.cameraFar = in.camera_far;
    prepare.cameraFovAngleVertical = in.vertical_fov;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = AmdFfx::Image2D(in.depth, in.depth_format, in.render_size.width,
                                    in.render_size.height, false, read);
    prepare.motionVectors = AmdFfx::Image2D(in.motion_vectors, in.motion_format,
                                            in.render_size.width, in.render_size.height, false,
                                            read);
    const auto copy3 = [](const std::array<f32, 3>& from, float* to) {
        to[0] = from[0];
        to[1] = from[1];
        to[2] = from[2];
    };
    copy3(in.camera->position, prepare.cameraPosition);
    copy3(in.camera->up, prepare.cameraUp);
    copy3(in.camera->right, prepare.cameraRight);
    copy3(in.camera->forward, prepare.cameraForward);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([prepare, context = &state.frame_gen_context](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier inputs{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, inputs);
        ffxDispatchDescFrameGenerationPrepareV2 dispatch = prepare;
        dispatch.commandList = *cmdbuf;
        AmdFfx::Scope ffx_scope{"ffxDispatch(frame generation prepare)"};
        if (const auto result = AmdFfx::Api().dispatch(context, &dispatch.header);
            result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "AMD FSR frame generation: prepare returned {}", result);
        }
    });
    // AMD's backend bound its own pipelines and descriptor sets.
    scheduler.InvalidateState();
}

AmdFrameGenPacing AmdFrameGen::Pacing() const {
    auto& state = *impl;
    AmdFrameGenPacing pacing{};
    if (state.swapchain_context != nullptr) {
        pacing.swapchain_provider = AmdFfx::ProviderName(state.swapchain_context);
    }
    if (state.frame_gen_context != nullptr) {
        pacing.frame_gen_provider = AmdFfx::ProviderName(state.frame_gen_context);
    }
    pacing.driver = std::string{state.device.GetDriverInfo()};
    pacing.rdna4 = state.device.GetModelName().find("RX 9") != std::string_view::npos;
    pacing.driver_26_10_1 = AdrenalinVersion(state.device.GetDriverInfo()) >= 261001;
    pacing.swapchain_4 = AmdFfx::IsVersion4OrNewer(pacing.swapchain_provider);
    pacing.vsync_off = state.vsync_off;
    pacing.covers_monitor = state.covers_monitor;
    static const bool hags = HagsEnabled();
    pacing.hags = hags;
    pacing.frame_generation = state.generating;
    if (state.functions.pOutGetLastPresentCountFFXAPI != nullptr && state.swapchain != nullptr) {
        pacing.presents = state.functions.pOutGetLastPresentCountFFXAPI(state.swapchain);
    }
    return pacing;
}

} // namespace Vulkan
