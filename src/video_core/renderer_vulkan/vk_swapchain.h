// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/present_mode_policy.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {

class AmdFrameGen;
class Device;
class Scheduler;

class Swapchain {
public:
    /// Creates the initial SDR swapchain without probing HDR encodings.
    /// Throws on startup failure rather than constructing an unusable presenter.
    /// @param window The native window the surface shows in (an HWND on Windows), for the
    ///               display's refresh rate.
    explicit Swapchain(
        VkSurfaceKHR_T* surface,
        void* window,
        const Device& device,
        Scheduler& scheduler,
        u32 width,
        u32 height);
    ~Swapchain();

    /// Creates (or recreates) the swapchain; returns false when deferred for zero extent.
    bool Create(
        VkSurfaceKHR_T* surface,
        u32 width,
        u32 height,
        VkFormat requested_format = VK_FORMAT_B8G8R8A8_UNORM,
        VkColorSpaceKHR requested_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

    /// Acquires the next image in the swapchain, waits as needed. Returns true when no image was
    /// acquired (out of date) and the swapchain has to be recreated first.
    /// `may_flush`: false on the async present thread (see Scheduler::Wait).
    bool AcquireNextImage(bool may_flush = true);

    /// Presents the rendered image to the swapchain.
    void Present(VkSemaphore render_semaphore);

    /// Returns true when the swapchain needs to be recreated.
    bool NeedsRecreation() const {
        return Handle() == VK_NULL_HANDLE || IsSubOptimal() || NeedsPresentModeUpdate();
    }

    bool IsCreated() const {
        return Handle() != VK_NULL_HANDLE;
    }

    /// AMD's frame generation swapchain while it presents (null otherwise).
    [[nodiscard]] AmdFrameGen* GetAmdFrameGen() const noexcept {
#ifdef _WIN32
        return amd_swapchain != VK_NULL_HANDLE ? amd_frame_gen.get() : nullptr;
#else
        return nullptr;
#endif
    }

    [[nodiscard]] VkPresentModeKHR GetPresentMode() const noexcept {
        return present_mode;
    }

    /// Returns true when the swapchain is outdated.
    bool IsOutDated() const {
        return is_outdated;
    }

    /// Returns true when the swapchain is suboptimal.
    bool IsSubOptimal() const {
        return is_suboptimal;
    }

    VkExtent2D GetSize() const {
        return extent;
    }

    std::size_t GetImageCount() const {
        return image_count;
    }

    std::size_t GetImageIndex() const {
        return image_index;
    }

    std::size_t GetFrameIndex() const {
        return frame_index;
    }

    /// Lichii's own swapchain, not AMD's (which has no present ids or waits and
    /// paces its own presents).
    [[nodiscard]] bool IsNativeSwapchain() const noexcept {
#ifdef _WIN32
        return amd_swapchain == VK_NULL_HANDLE;
#else
        return true;
#endif
    }

    VkImage GetImageIndex(std::size_t index) const {
        return images[index];
    }

    VkImage CurrentImage() const {
        return images[image_index];
    }

    VkFormat GetImageViewFormat() const {
        return image_view_format;
    }

    VkFormat GetImageFormat() const {
        return surface_format.format;
    }

    /// Hands the display the HDR10 mastering metadata (VK_EXT_hdr_metadata) of an HDR10 swapchain.
    void SetHdrMetadata(u16 peak_luminance, u16 reference_white) const;

    /// Returns the cached SDR surface format, including while an HDR swapchain is active.
    VkFormat GetSdrImageFormat() const {
        return sdr_image_format.load();
    }

    /// Returns the producer format requested by the latest creation attempt.
    VkFormat GetRequestedFormat() const {
        return requested_format;
    }

    VkColorSpaceKHR GetRequestedColorSpace() const {
        return requested_color_space;
    }

    VkSemaphore CurrentPresentSemaphore() const {
        return *present_semaphores[frame_index];
    }

    VkSemaphore CurrentRenderSemaphore() const {
        return *render_semaphores[image_index];
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    VkExtent2D GetExtent() const {
        return extent;
    }

    /// Presents per second the game should be held below (VRR frame cap), 0 for none.
    /// Read from other threads.
    u32 GetFrameCap() const {
        return frame_cap.load(std::memory_order_relaxed);
    }

private:
    void CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities);
    void CreateSemaphores();
    void CreateImageViews();

    void Destroy();

    [[nodiscard]] VkSwapchainKHR Handle() const noexcept {
#ifdef _WIN32
        return amd_swapchain != VK_NULL_HANDLE ? amd_swapchain : *swapchain;
#else
        return *swapchain;
#endif
    }

    bool NeedsPresentModeUpdate() const;
    PresentModeSupport PresentModes() const;
    bool SurfaceSupportsPresentWait() const;

    VkSurfaceKHR_T* surface;
    void* window;

    const Device& device;
    Scheduler& scheduler;

    vk::SwapchainKHR swapchain;
#ifdef _WIN32
    /// AMD's frame generation swapchain (present/amd_frame_gen.h) in place of `swapchain`.
    std::unique_ptr<AmdFrameGen> amd_frame_gen;
    VkSwapchainKHR amd_swapchain{};
    bool amd_failed{}; ///< AMD's swapchain could not be made: not tried again
#endif

    std::size_t image_count{};
    std::vector<VkImage> images;
    std::vector<u64> resource_ticks;
    std::vector<vk::Semaphore> present_semaphores;
    std::vector<vk::Semaphore> render_semaphores;

    u32 width;
    u32 height;

    u32 image_index{};
    u32 frame_index{};

    VkFormat image_view_format{};
    VkExtent2D extent{};
    VkPresentModeKHR present_mode{};
    VkSurfaceFormatKHR surface_format{};
    std::atomic<VkFormat> sdr_image_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkFormat requested_format{VK_FORMAT_B8G8R8A8_UNORM};
    VkColorSpaceKHR requested_color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    bool has_imm{false};
    bool has_mailbox{false};
    bool has_fifo_relaxed{false};
    bool has_fifo_latest_ready{false};
    u32 refresh_hz{};
    std::atomic<u32> frame_cap{};
    bool present_wait{}; ///< Presents carry ids (VK_KHR_present_id2) and are waited for.
    u64 present_id{};    ///< Id of the last present.

    bool is_outdated{};
    bool is_suboptimal{};
};

} // namespace Vulkan
