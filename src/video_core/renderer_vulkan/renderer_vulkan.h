// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include "common/dynamic_library.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#ifdef HAS_LSFG
#include "video_core/perf_overlay.h"
#include "video_core/renderer_vulkan/present/frame_gen.h"
#include "video_core/renderer_vulkan/present/perf_overlay_pass.h"
#endif
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_turbo_mode.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Memory {
class Memory;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface);

class RendererVulkan final : public VideoCore::RendererBase {
public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                            std::unique_ptr<Core::Frontend::GraphicsContext> context_);
    ~RendererVulkan() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.GetDriverName();
    }

    [[nodiscard]] u32 HostFrameCap() const override {
        return swapchain.GetFrameCap();
    }

    void SetGuestPresentRate(u32 hz) override {
        present_manager.SetGuestPresentRate(hz);
    }

    [[nodiscard]] VideoCore::FrameGenStatus GetFrameGenStatus() const override {
#ifdef HAS_LSFG
        return frame_gen.CurrentStatus();
#else
        return {.state = VideoCore::FrameGenState::UnsupportedBuild};
#endif
    }

    [[nodiscard]] VideoCore::PresentStatus GetPresentStatus() const override;

    // Enhanced platform-specific initialization
    void InitializePlatformSpecific();

private:
    Common::DkbFrameTrace::RateLimit trace_cpu_rate;
    std::chrono::steady_clock::time_point trace_last_frame{};
    u64 trace_frame{};
    VideoCore::PerfOverlay::Stats perf_overlay_stats;
    f32 perf_overlay_white{};

    void RefreshPerfOverlay(std::chrono::steady_clock::time_point now, bool hdr_output);

    void InterpolateFrames(Frame* prev_frame, Frame* curr_frame);
    Frame* previous_frame = nullptr;  // Store the previous frame for interpolation
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer command_buffer);
    void Report() const;
    /// Every half second: the window's monitor (refresh rate, fullscreen) and the status.
    void UpdatePacing();

    vk::Buffer RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                              const Layout::FramebufferLayout& layout, VkFormat format,
                              VkDeviceSize buffer_size, bool hdr_output = false);
    void RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers);
    void RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers);

    std::vector<Tegra::FramebufferConfig> applet_capture_layers;
    std::vector<Tegra::FramebufferConfig> screenshot_layer_scratch;

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;
#ifdef EGLANTINE_STREAMLINE
    bool streamline_started; ///< NVIDIA Streamline starts before the instance.
#endif

    // Keep original handles for compatibility with existing code
    vk::Instance instance;

    vk::DebugUtilsMessenger debug_messenger;

    vk::SurfaceKHR surface;

    Device device;
    MemoryAllocator memory_allocator;
    StateTracker state_tracker;
    Scheduler scheduler;
    Swapchain swapchain;
    PresentManager present_manager;
    BlitScreen blit_swapchain;
    BlitScreen blit_capture;
    BlitScreen blit_applet;
    RasterizerVulkan rasterizer;
#ifdef HAS_LSFG
    FrameGen frame_gen;
#endif
    std::optional<TurboMode> turbo_mode;

    Frame applet_frame;

    bool covers_monitor{};
    u32 refresh_hz{};
    std::chrono::steady_clock::time_point display_checked{};
    mutable std::mutex present_status_mutex;
    VideoCore::PresentStatus present_status;
    std::optional<bool> logged_driver_pacing;
};

} // namespace Vulkan
