// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <thread>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// AMD FSR 4 upscaling (the ML upscaler; FP8 model on RDNA 4, chosen by AMD's provider) through
/// AMD's runtime DLLs (vulkan_common/amd_ffx.h). The dispatch is recorded into the scheduler's
/// command buffer. Contexts are slow to create, so they are made on a worker thread and Prepare
/// says when one is ready; callers keep their fallback until then.
class AmdFsr4Upscaler {
public:
    struct Image {
        VkImage image{};
        VkFormat format{};
        VkExtent2D extent{}; ///< Of the image itself.
        u32 layer{};         ///< Array layer to read (copied to a plain image when not 0).
    };
    struct Inputs {
        Image color;          ///< At the render size, GENERAL layout.
        Image depth;          ///< D32, [0,1] not inverted, GENERAL layout.
        Image motion_vectors; ///< RG16F at the render size, GENERAL layout.
        std::optional<Image> reactive; ///< R16F/R8 mask, 1 = take the current colour.
        VkExtent2D render_size{};
        VkExtent2D upscale_size{};
        std::array<f32, 2> jitter{};              ///< In render pixels, DLSS's convention.
        std::array<f32, 2> motion_vector_scale{}; ///< To render pixels.
        f32 frame_time_ms{};
        bool reset{};
        bool sharpen{};
        f32 sharpness{}; ///< 0 to 1, FSR 4's own sharpening pass.
        f32 camera_near{};
        f32 camera_far{};
        f32 vertical_fov{};
        std::optional<f32> exposure; ///< HDR: the tonemap's exposure; none: FSR's auto exposure.
    };

    explicit AmdFsr4Upscaler(const Device& device, Scheduler& scheduler,
                             MemoryAllocator& memory_allocator);
    ~AmdFsr4Upscaler();

    AmdFsr4Upscaler(const AmdFsr4Upscaler&) = delete;
    AmdFsr4Upscaler& operator=(const AmdFsr4Upscaler&) = delete;

    /// AMD's runtime offers an FSR 4 upscaler for this GPU (checked once).
    [[nodiscard]] static bool IsAvailable(const Device& device);

    /// Makes sure a context for these sizes and colour range exists or is being made. True once
    /// it is ready to record.
    bool Prepare(VkExtent2D max_render_size, VkExtent2D upscale_size, bool hdr);

    /// Records the upscale (after Prepare returned true). False when AMD's dispatch failed.
    bool Upscale(const Inputs& inputs);

    [[nodiscard]] bool Failed() const noexcept {
        return failed.load(std::memory_order_relaxed);
    }
    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }
    [[nodiscard]] VkImage OutputImage() const noexcept {
        return *output;
    }

private:
    struct Context;
    struct Staged {
        vk::Image image;
        VkFormat format{};
        VkExtent2D extent{};
    };

    void Destroy(std::unique_ptr<Context>& context);
    /// A plain copy of `source`'s layer at `size` (FFX takes whole images, layer 0).
    VkImage Stage(Staged& staged, const Image& source, VkExtent2D size,
                  VkImageAspectFlags aspect);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;

    std::unique_ptr<Context> current;
    std::unique_ptr<Context> staging;
    std::jthread staging_thread;
    std::atomic<bool> staging_done{};

    vk::Image output;
    vk::ImageView output_view;
    VkExtent2D output_extent{};
    vk::Image exposure;
    Staged staged_color;
    Staged staged_depth;
    /// Ticks of the dispatches still on the GPU: AMD's backend keeps a few frames of state.
    std::deque<u64> ticks;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
