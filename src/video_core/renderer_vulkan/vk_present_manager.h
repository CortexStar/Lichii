// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <boost/container/deque.hpp>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "video_core/renderer_vulkan/present_cadence.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class EmuWindow;
} // namespace Core::Frontend

namespace Vulkan {

class Device;
class Scheduler;
class Swapchain;

struct Frame {
    u32 width;
    u32 height;
    u32 index;
    // Requested presentation view format and underlying image format may differ for SDR.
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkFormat image_format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    vk::Image image;
    vk::ImageView image_view;
    vk::ImageView storage_view;
    vk::Framebuffer framebuffer;
    vk::CommandBuffer cmdbuf;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    u64 ready_tick{}; ///< Scheduler tick of the submission that renders it
    bool storage_capable{};
    /// The button change the guest frame shown sampled (Common::InputLatency), 0 for none.
    u64 input_ns{};
};

class PresentManager {
public:
    PresentManager(const vk::Instance& instance,
                   Core::Frontend::EmuWindow& render_window,
                   const Device& device,
                   MemoryAllocator& memory_allocator,
                   Scheduler& scheduler,
                   Swapchain& swapchain,
                   vk::SurfaceKHR& surface);
    ~PresentManager();

    /// Returns the last used presentation frame
    Frame* GetRenderFrame();

    /// Pushes a frame for presentation
    void Present(Frame* frame);

    /// Recreates the present frame to match the provided parameters
    void RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                       VkRenderPass rd, bool storage);

    [[nodiscard]] bool NeedsStorage(const Frame* frame, bool required) const;

    /// Selects the backing image format, preserving the cached SDR format or explicit HDR format.
    [[nodiscard]] VkFormat GetFrameImageFormat(VkFormat image_view_format) const;

    /// Waits for the present thread to finish presenting all queued frames.
    void WaitPresent();

    /// How many additional frames can be queued without stalling the render thread
    [[nodiscard]] size_t MaxExtraFrames() const;

    /// The rate the guest's presents are paced at, 0 when they are not (see PresentCadence).
    void SetGuestPresentRate(u32 hz) {
        guest_present_rate.store(hz, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t SwapchainImageCount() const { return swapchain_image_count; }
    [[nodiscard]] VkFormat SwapchainImageFormat() const { return swapchain_image_format; }

private:
    enum class TransferMode { Unsupported, Copy, Blit };

    void PresentThread(std::stop_token token);

    /// Presents one frame on the next tick of the cadence at `rate` ticks per second: the oldest
    /// queued frame the GPU has finished, or the last presented one again.
    void PresentOnTick(std::stop_token token, u32 rate);

    /// Returns the frame kept for re-presenting to the free queue.
    void ReleaseHeldFrame();

    /// Logs the cadence's work every 10 s.
    void ReportCadence(PresentCadence::Clock::time_point now, u32 rate);

    void CopyToSwapchain(Frame* frame);

    void CopyToSwapchainImpl(Frame* frame);

    // Returns false after discarding the frame if creation or transfer cannot proceed.
    bool RecreateSwapchain(Frame* frame);

    /// Caches a copy, blit, or unsupported transfer decision for this frame and active swapchain.
    void ConfigureTransfer(const Frame* frame);

    void DiscardFrame(Frame* frame);

    /// Sends the HDR10 metadata of the monitor's current state to an HDR10 swapchain.
    void UpdateHdrMetadata();

    void SetImageCount();

    /// Waits until the copy that last waited on the current swapchain slot's acquire semaphore has
    /// run (present thread only).
    void WaitForSlot();

private:
    const vk::Instance& instance;
    Core::Frontend::EmuWindow& render_window;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    Swapchain& swapchain;
    vk::SurfaceKHR& surface;
    vk::CommandPool cmdpool;
    std::vector<Frame> frames;
    boost::container::deque<Frame*> present_queue;
    boost::container::deque<Frame*> free_queue;
    std::condition_variable_any frame_cv;
    std::condition_variable free_cv;
    std::mutex swapchain_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    VkFormat transfer_source_format{VK_FORMAT_UNDEFINED};
    TransferMode transfer_mode{TransferMode::Unsupported};
    u64 hdr_metadata_generation{}; ///< DisplayHdrMonitor generation of the metadata sent
    bool storage_supported;
    bool use_present_thread;
    std::atomic<std::size_t> image_count{};
    std::atomic<std::size_t> swapchain_image_count{};
    std::atomic<VkFormat> swapchain_image_format{};

    /// Per swapchain slot (Swapchain::GetFrameIndex): signaled once the present thread's copy
    /// that waited on that slot's acquire semaphore has run.
    std::vector<vk::Fence> slot_fences;

    std::atomic<u32> guest_present_rate{};
    PresentCadence cadence;
    Common::Event cadence_wake; ///< Set to end a tick wait early (shutdown)
    Frame* held_frame{};      ///< Last presented frame, presented again on ticks without a new one
    bool presenting_repeat{}; ///< CopyToSwapchain is presenting held_frame again
    struct CadenceStats {
        PresentCadence::Clock::time_point since{};
        u32 frames{}, repeats{}, late_frames{}, max_queued{};
        s64 max_late_us{};
    } cadence_stats;
};

} // namespace Vulkan
