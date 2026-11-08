// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "video_core/oot_camera.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;
struct PerfOverlayDraw;

/// What Lichii knows of AMD's frame pacing for the FG swapchain. AMD's driver meters the flips
/// itself (Driver-Assisted Frame Pacing, frame-interpolation-swap-chain.md:206-229) when all of
/// these hold; AMD's API reports neither the request nor the mode (notes/fg-hwflip.md).
struct AmdFrameGenPacing {
    std::string swapchain_provider;  ///< FG swapchain version AMD's runtime picked
    std::string frame_gen_provider;  ///< frame generation version
    std::string driver;              ///< VkPhysicalDeviceDriverProperties::driverInfo
    bool rdna4{};                    ///< Radeon RX 9000 (FG swapchain 4.0.0 needs it)
    bool driver_26_10_1{};           ///< Adrenalin 26.10.1 or newer
    bool swapchain_4{};              ///< the FG swapchain provider is 4.x
    bool vsync_off{};                ///< IMMEDIATE present mode
    bool covers_monitor{};           ///< the window covers its monitor (fullscreen)
    bool hags{};                     ///< hardware-accelerated GPU scheduling on
    bool frame_generation{};         ///< frames are being generated
    u64 presents{};                  ///< presents of the FG swapchain since its creation
    [[nodiscard]] bool DriverPacingExpected() const {
        return rdna4 && driver_26_10_1 && swapchain_4 && vsync_off && covers_monitor && hags;
    }
};

/// AMD FSR frame generation presented through AMD's frame generation swapchain for Vulkan
/// (ffxCreateContextDescFrameGenerationSwapChainVK), so AMD paces the presents and, on RDNA 4
/// with Adrenalin 26.10.1+, the driver meters the flips. AMD's swapchain replaces
/// vkCreateSwapchainKHR, vkAcquireNextImageKHR and vkQueuePresentKHR; it dispatches frame
/// generation itself inside present (frameGenerationCallback) on its async compute queue.
/// Per real frame, on the thread that also presents: Configure (frame N), then Prepare (frame N's
/// depth, motion and camera) in the scheduler's command buffer, then Present (frame N). Windows
/// only (AMD's runtime).
class AmdFrameGen {
public:
    struct PrepareInputs {
        VkImage depth{};
        VkFormat depth_format{};
        u32 depth_layer{};
        VkImage motion_vectors{};
        VkFormat motion_format{};
        VkExtent2D render_size{};
        std::array<f32, 2> jitter{};
        std::array<f32, 2> motion_vector_scale{};
        bool reset{};
        f32 camera_near{};
        f32 camera_far{};
        f32 vertical_fov{};
        std::optional<VideoCommon::OotCamera::Basis> camera;
    };

    explicit AmdFrameGen(const Device& device, Scheduler& scheduler);
    ~AmdFrameGen();

    AmdFrameGen(const AmdFrameGen&) = delete;
    AmdFrameGen& operator=(const AmdFrameGen&) = delete;

    /// Frame generation is set to AMD FSR frame generation.
    [[nodiscard]] static bool Wanted();
    /// AMD's runtime and the device can run it (checked once).
    [[nodiscard]] static bool IsAvailable(const Device& device);
    /// The instance presenting now (null when none), for the producers of its inputs.
    [[nodiscard]] static AmdFrameGen* Active();

    /// Creates AMD's swapchain in place of `ci`'s, or recreates it in place. VK_SUCCESS or the
    /// error; `swapchain` then holds AMD's swapchain.
    VkResult CreateSwapchain(const VkSwapchainCreateInfoKHR& ci, VkSwapchainKHR& swapchain);
    [[nodiscard]] std::vector<VkImage> GetImages(VkSwapchainKHR swapchain) const;
    VkResult AcquireNextImage(VkSwapchainKHR swapchain, u64 timeout, VkSemaphore semaphore,
                              u32* image_index) const;
    /// Waits until AMD's present threads have sent every present they hold (they still wait on
    /// Lichii's semaphores until then).
    void WaitForPresents();

    /// AMD's replacement of vkSetHdrMetadataEXT.
    void SetHdrMetadata(VkSwapchainKHR swapchain, const VkHdrMetadataEXT& metadata) const;
    /// Presents on the game queue; configures frame generation off for a frame without inputs.
    VkResult Present(VkQueue game_queue, const VkPresentInfoKHR& present_info);

    /// The area of the presented image that shows the game (the rest stays as presented).
    void SetGenerationRect(s32 left, s32 top, s32 width, s32 height);
    /// Whether the window covers its monitor and the present mode is IMMEDIATE (pacing status).
    void SetPresentationState(bool covers_monitor, bool vsync_off);

    /// Draws the presenter's own UI (the performance overlay) into the UI resource AMD's swapchain
    /// blends over every real and generated frame, so frame generation never interpolates it.
    /// Without `draw` the swapchain composes no UI.
    void ComposeUi(MemoryAllocator& allocator, VkFormat format, const PerfOverlayDraw* draw);

    /// The game's scene without its HUD, for frame generation to find the HUD in the presented
    /// image (frame-interpolation-swap-chain.md, "UI Composition", method 3). Builds this frame's
    /// HUD-less back buffer from the presented frame `frame_image` (`frame_format`, the back
    /// buffer's format and size) with the scene copied over `rect`: only where the scene
    /// (linear float) is shown 1:1 and the back buffer is 8-bit sRGB-encoded; otherwise the
    /// presented frame itself (no HUD found). Recorded before the frame is presented.
    void ComposeHudless(MemoryAllocator& allocator, VkImage frame_image, VkFormat frame_format,
                        VkExtent2D frame_extent, VkImage scene, VkExtent2D scene_extent,
                        bool scene_fresh, VkRect2D rect);

    /// Configures frame N and records its prepare pass (depth, motion, camera).
    void Prepare(Scheduler& scheduler, const PrepareInputs& inputs);

    [[nodiscard]] AmdFrameGenPacing Pacing() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
