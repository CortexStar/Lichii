// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#ifdef __ANDROID__
#include <android/api-level.h>
#endif

#include "common/display_refresh.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "video_core/renderer_vulkan/present_mode_policy.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "vulkan/vulkan_core.h"

namespace Vulkan {

namespace {

VkSurfaceFormatKHR ChooseSwapSurfaceFormat(vk::Span<VkSurfaceFormatKHR> formats,
                                         VkFormat requested_format,
                                         VkColorSpaceKHR requested_color_space) {
    if (requested_format == VK_FORMAT_R16G16B16A16_SFLOAT ||
        requested_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
        requested_color_space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        // A swapchain may only use a pair the surface lists
        // (VUID-VkSwapchainCreateInfoKHR-imageFormat-01273).
        const bool listed = std::ranges::any_of(formats, [&](const VkSurfaceFormatKHR& format) {
            return format.format == requested_format &&
                   format.colorSpace == requested_color_space;
        });
        if (listed) {
            return {
                .format = requested_format,
                .colorSpace = requested_color_space,
            };
        }
        LOG_ERROR(Render_Vulkan, "The surface has no {} {} format, presenting SDR",
                  string_VkFormat(requested_format), string_VkColorSpaceKHR(requested_color_space));
    }
    if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        VkSurfaceFormatKHR format;
        format.format = VK_FORMAT_B8G8R8A8_UNORM;
        format.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return format;
    }
    const auto& found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.format == VK_FORMAT_B8G8R8A8_UNORM &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (found != formats.end()) {
        return *found;
    }
    const auto sdr = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    return sdr != formats.end() ? *sdr : formats[0];
}

PresentPolicy ChoosePresentPolicyForSettings(u32 refresh_hz, PresentModeSupport support) {
    const bool locked = Settings::values.use_speed_limit.GetValue() &&
                        Settings::values.current_speed_mode.GetValue() != Settings::SpeedMode::Turbo;
    return ChoosePresentPolicy(Settings::values.vsync_mode.GetValue(), !locked,
                               Settings::values.frame_gen.GetValue(),
                               Settings::values.variable_refresh_rate.GetValue(), refresh_hz,
                               support);
}

// How long the present thread waits for the previous present to reach the display. Long enough
// for any refresh rate, short enough that an occluded window that never shows a present does not
// stall presentation.
constexpr u64 PresentWaitTimeoutNs = 100'000'000;

VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, u32 width, u32 height) {
    constexpr auto undefined_size{(std::numeric_limits<u32>::max)()};
    if (capabilities.currentExtent.width != undefined_size) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent;
    extent.width = (std::max)(capabilities.minImageExtent.width,
                            (std::min)(capabilities.maxImageExtent.width, width));
    extent.height = (std::max)(capabilities.minImageExtent.height,
                             (std::min)(capabilities.maxImageExtent.height, height));
    return extent;
}

VkCompositeAlphaFlagBitsKHR ChooseAlphaFlags(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown composite alpha flags value {:#x}",
                  capabilities.supportedCompositeAlpha);
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
}

} // Anonymous namespace

Swapchain::Swapchain(
    VkSurfaceKHR_T* surface_,
    void* window_,
    const Device& device_,
    Scheduler& scheduler_,
    u32 width_,
    u32 height_)
    : surface(surface_)
    , window{window_}
    , device{device_}
    , scheduler{scheduler_}
{
    if (!Create(surface, width_, height_)) {
        LOG_ERROR(Render_Vulkan, "Initial swapchain creation failed: surface has zero extent");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
}

Swapchain::~Swapchain() = default;

bool Swapchain::Create(
    VkSurfaceKHR_T* surface_,
    u32 width_,
    u32 height_,
    VkFormat requested_format_,
    VkColorSpaceKHR requested_color_space_)
{
    is_outdated = false;
    // Keep recreation pending until creation succeeds, including zero-extent surfaces.
    is_suboptimal = true;
    width = width_;
    height = height_;
    surface = surface_;
    requested_format = requested_format_;
    requested_color_space = requested_color_space_;

    const auto physical_device = device.GetPhysical();
    const auto capabilities{physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface))};
    if (capabilities.maxImageExtent.width == 0 || capabilities.maxImageExtent.height == 0) {
        return false;
    }

    // Submitted work may still wait on the old acquire/present semaphores; destroying them under
    // it loses the device on MoltenVK (window moved to a display with a different scale).
    if (swapchain) {
        std::scoped_lock lock{scheduler.submit_mutex};
        const VkResult result = device.GetLogical().WaitIdle();
        if (result == VK_ERROR_DEVICE_LOST) {
            device.ReportLoss();
        }
        vk::Check(result);
    }
    Destroy();

    CreateSwapchain(capabilities);
    CreateSemaphores();
    is_suboptimal = false;

    resource_ticks.clear();
    resource_ticks.resize(image_count);
    return true;
}

bool Swapchain::AcquireNextImage(bool may_flush) {
    // Hold at most one present between the GPU and the display: the frame acquired now waits until
    // the one before the last present is on screen, so presents never queue up behind the display.
    if (present_wait && present_id > 1) {
        const VkResult wait = device.GetLogical().WaitForPresent2KHR(*swapchain, present_id - 1,
                                                                    PresentWaitTimeoutNs);
        switch (wait) {
        case VK_SUCCESS:
        case VK_TIMEOUT:
        case VK_SUBOPTIMAL_KHR:
            break;
        case VK_ERROR_OUT_OF_DATE_KHR:
            is_outdated = true;
            return true;
        case VK_ERROR_SURFACE_LOST_KHR:
            vk::Check(wait);
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            vk::Check(wait);
            break;
        default:
            LOG_ERROR(Render_Vulkan, "vkWaitForPresent2KHR returned {}", string_VkResult(wait));
            break;
        }
    }
    const VkResult result = device.GetLogical().AcquireNextImageKHR(
        *swapchain, (std::numeric_limits<u64>::max)(), *present_semaphores[frame_index],
        VK_NULL_HANDLE, &image_index);
    switch (result) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        is_suboptimal = true;
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));
        break;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // No image was acquired, so image_index is stale: the caller recreates and tries again.
        return true;
    }

    const auto wait_with_frame_pacing = [this, may_flush] {
    switch (Settings::values.frame_pacing_mode.GetValue()) {
    case Settings::FramePacingMode::Target_Auto:
        scheduler.Wait(resource_ticks[image_index], 0.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_30:
        scheduler.Wait(resource_ticks[image_index], 30.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_60:
        scheduler.Wait(resource_ticks[image_index], 60.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_90:
        scheduler.Wait(resource_ticks[image_index], 90.0, may_flush);
        break;
    case Settings::FramePacingMode::Target_120:
        scheduler.Wait(resource_ticks[image_index], 120.0, may_flush);
        break;
    }
    };

#ifdef __ANDROID__
    if (android_get_device_api_level() >= 30) {
        scheduler.Wait(resource_ticks[image_index], 0.0, may_flush);
    } else {
        wait_with_frame_pacing();
    }
#else
    wait_with_frame_pacing();
#endif

    resource_ticks[image_index] = scheduler.CurrentTick();

    // A suboptimal acquire still acquired an image and signals its semaphore: present it, and let
    // NeedsRecreation() recreate at the next frame.
    return false;
}

void Swapchain::Present(VkSemaphore render_semaphore) {
    const auto present_queue{device.GetPresentQueue()};
    const u64 next_present_id = present_id + 1;
    const VkPresentId2KHR present_id_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .pNext = nullptr,
        .swapchainCount = 1,
        .pPresentIds = &next_present_id,
    };
    const VkPresentInfoKHR present_info{
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = present_wait ? &present_id_info : nullptr,
        .waitSemaphoreCount = render_semaphore ? 1U : 0U,
        .pWaitSemaphores = &render_semaphore,
        .swapchainCount = 1,
        .pSwapchains = swapchain.address(),
        .pImageIndices = &image_index,
        .pResults = nullptr,
    };
    std::scoped_lock lock{scheduler.submit_mutex};
    if (present_wait) {
        // The id belongs to this present whatever it returns.
        present_id = next_present_id;
    }
    switch (const VkResult result = present_queue.Present(present_info)) {
    case VK_SUCCESS:
        break;
    case VK_SUBOPTIMAL_KHR:
        LOG_DEBUG(Render_Vulkan, "Suboptimal swapchain");
        break;
    case VK_ERROR_OUT_OF_DATE_KHR:
        is_outdated = true;
        break;
    case VK_ERROR_SURFACE_LOST_KHR:
        vk::Check(result);
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));
        break;
    }
    ++frame_index;
    if (frame_index >= image_count) {
        frame_index = 0;
    }
}

void Swapchain::CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities) {
    const auto physical_device{device.GetPhysical()};
    const auto formats{physical_device.GetSurfaceFormatsKHR(VkSurfaceKHR(surface))};
    const auto present_modes = physical_device.GetSurfacePresentModesKHR(VkSurfaceKHR(surface));

    // Keep the SDR choice available even while the active swapchain is HDR.
    sdr_image_format = ChooseSwapSurfaceFormat(formats, VK_FORMAT_B8G8R8A8_UNORM,
                                              VK_COLOR_SPACE_SRGB_NONLINEAR_KHR).format;

    has_mailbox = std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_MAILBOX_KHR)
                  != present_modes.end();
    has_imm = std::find(present_modes.begin(), present_modes.end(),
                        VK_PRESENT_MODE_IMMEDIATE_KHR) != present_modes.end();
    has_fifo_relaxed = std::find(present_modes.begin(), present_modes.end(),
                                 VK_PRESENT_MODE_FIFO_RELAXED_KHR) != present_modes.end();
    has_fifo_latest_ready = device.IsKhrPresentModeFifoLatestReadyEnabled() &&
                            std::find(present_modes.begin(), present_modes.end(),
                                      VK_PRESENT_MODE_FIFO_LATEST_READY_KHR) != present_modes.end();
    refresh_hz = Common::QueryDisplayRefreshRate(window).value_or(0);

    const VkCompositeAlphaFlagBitsKHR alpha_flags{ChooseAlphaFlags(capabilities)};
    surface_format = ChooseSwapSurfaceFormat(formats, requested_format, requested_color_space);
    const PresentPolicy policy = ChoosePresentPolicyForSettings(refresh_hz, PresentModes());
    present_mode = policy.mode;
    frame_cap.store(policy.frame_cap, std::memory_order_relaxed);
    present_wait = SurfaceSupportsPresentWait();

    u32 requested_image_count{capabilities.minImageCount + 1};
    // Ensure Triple buffering if possible.
    if (capabilities.maxImageCount > 0) {
        if (requested_image_count > capabilities.maxImageCount) {
            requested_image_count = capabilities.maxImageCount;
        } else {
            requested_image_count =
                (std::max)(requested_image_count, (std::min)(3U, capabilities.maxImageCount));
        }
    } else {
        requested_image_count = (std::max)(requested_image_count, 3U);
    }
    VkSwapchainCreateInfoKHR swapchain_ci{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = nullptr,
        .flags = 0,
        .surface = VkSurfaceKHR(surface),
        .minImageCount = requested_image_count,
        .imageFormat = surface_format.format,
        .imageColorSpace = surface_format.colorSpace,
        .imageExtent = {},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
#ifdef __ANDROID__
        // On Android, do not allow surface rotation to deviate from the frontend.
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
#else
        .preTransform = capabilities.currentTransform,
#endif
        .compositeAlpha = alpha_flags,
        .presentMode = present_mode,
        .clipped = VK_FALSE,
        .oldSwapchain = VkSwapchainKHR{},
    };
    const u32 graphics_family{device.GetGraphicsFamily()};
    const u32 present_family{device.GetPresentFamily()};
    const std::array<u32, 2> queue_indices{graphics_family, present_family};
    if (graphics_family != present_family) {
        swapchain_ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_ci.queueFamilyIndexCount = static_cast<u32>(queue_indices.size());
        swapchain_ci.pQueueFamilyIndices = queue_indices.data();
    }
    // According to Vulkan spec, when using VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR,
    // the base format (imageFormat) MUST be included in pViewFormats
    const std::array view_formats{
        swapchain_ci.imageFormat,  // Base format MUST be first
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB,
#ifdef __ANDROID__
        VK_FORMAT_R8G8B8A8_UNORM,  // Android may use RGBA
        VK_FORMAT_R8G8B8A8_SRGB,
#endif
    };
    VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    if (device.IsKhrSwapchainMutableFormatEnabled() &&
        surface_format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        format_list.pNext = std::exchange(swapchain_ci.pNext, &format_list);
        swapchain_ci.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
    }
    if (present_wait) {
        swapchain_ci.flags |=
            VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    }
    // Request the size again to reduce the possibility of a TOCTOU race condition.
    const auto updated_capabilities = physical_device.GetSurfaceCapabilitiesKHR(VkSurfaceKHR(surface));
    swapchain_ci.imageExtent = ChooseSwapExtent(updated_capabilities, width, height);
    // Don't add code within this and the swapchain creation.
    swapchain = device.GetLogical().CreateSwapchainKHR(swapchain_ci);

    extent = swapchain_ci.imageExtent;

    images = swapchain.GetImages();
    image_count = static_cast<u32>(images.size());
    LOG_INFO(Render_Vulkan,
             "Swapchain {}x{}, {} images, {}, display {} Hz{}, frame cap {}, present wait {}",
             extent.width, extent.height, image_count, string_VkPresentModeKHR(present_mode),
             refresh_hz, Settings::values.variable_refresh_rate.GetValue() ? " VRR" : "",
             policy.frame_cap, present_wait ? "on" : "off");
#ifdef __ANDROID__
    // Android is already ordered the same as Switch.
    image_view_format = VK_FORMAT_R8G8B8A8_UNORM;
#else
    image_view_format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
    if (surface_format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        image_view_format = surface_format.format;
    }
}

void Swapchain::CreateSemaphores() {
    present_semaphores.resize(image_count);
    std::ranges::generate(present_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
    render_semaphores.resize(image_count);
    std::ranges::generate(render_semaphores,
                          [this] { return device.GetLogical().CreateSemaphore(); });
}

void Swapchain::Destroy() {
    frame_index = 0;
    present_id = 0;
    present_semaphores.clear();
    render_semaphores.clear();
    swapchain.reset();
}

void Swapchain::SetHdrMetadata(u16 peak_luminance, u16 reference_white) const {
    if (!device.IsExtHdrMetadataEnabled() ||
        surface_format.colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        return;
    }
    // The game tone maps to the peak it was told about, so that is the brightest the stream
    // gets. Mastering primaries are BT.2020 with a D65 white (the colour space's own); the frame
    // average is not known.
    const VkHdrMetadataEXT metadata{
        .sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
        .pNext = nullptr,
        .displayPrimaryRed = {0.708f, 0.292f},
        .displayPrimaryGreen = {0.170f, 0.797f},
        .displayPrimaryBlue = {0.131f, 0.046f},
        .whitePoint = {0.3127f, 0.3290f},
        .maxLuminance = static_cast<float>(peak_luminance),
        .minLuminance = 0.0f,
        .maxContentLightLevel = static_cast<float>(peak_luminance),
        .maxFrameAverageLightLevel = 0.0f,
    };
    device.GetLogical().SetHdrMetadataEXT(*swapchain, metadata);
    LOG_INFO(Render_Vulkan, "HDR10 metadata: peak {} nits (reference white {} nits)",
             peak_luminance, reference_white);
}

PresentModeSupport Swapchain::PresentModes() const {
    return {
        .immediate = has_imm,
        .mailbox = has_mailbox,
        .fifo_relaxed = has_fifo_relaxed,
        .fifo_latest_ready = has_fifo_latest_ready,
    };
}

bool Swapchain::SurfaceSupportsPresentWait() const {
    if (!device.IsKhrPresentWait2Enabled()) {
        return false;
    }
    VkSurfaceCapabilitiesPresentWait2KHR wait_capabilities{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR,
        .pNext = nullptr,
        .presentWait2Supported = VK_FALSE,
    };
    VkSurfaceCapabilitiesPresentId2KHR id_capabilities{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,
        .pNext = &wait_capabilities,
        .presentId2Supported = VK_FALSE,
    };
    VkSurfaceCapabilities2KHR capabilities{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
        .pNext = &id_capabilities,
        .surfaceCapabilities = {},
    };
    return device.GetPhysical().GetSurfaceCapabilities2KHR(VkSurfaceKHR(surface), capabilities) &&
           id_capabilities.presentId2Supported && wait_capabilities.presentWait2Supported;
}

bool Swapchain::NeedsPresentModeUpdate() const {
    return present_mode != ChoosePresentPolicyForSettings(refresh_hz, PresentModes()).mode;
}

} // namespace Vulkan
