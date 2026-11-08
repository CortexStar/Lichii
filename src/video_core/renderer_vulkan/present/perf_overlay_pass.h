// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/perf_overlay.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {

class Device;
class MemoryAllocator;

/// What the presenter needs to draw the overlay this frame.
struct PerfOverlayInput {
    const VideoCore::PerfOverlay::Stats* stats{};
    float white_nits{}; ///< Text white on HDR outputs
};

struct PerfOverlayPushConstants {
    std::array<f32, 4> rect;         ///< Box corners in normalized device coordinates
    std::array<f32, 4> origin_scale; ///< Box top-left in framebuffer pixels, font pixel size
    std::array<f32, 4> text_color;
    std::array<f32, 4> graph_color;
    std::array<f32, 4> hitch_color;
    std::array<f32, 4> background;
};
static_assert(sizeof(PerfOverlayPushConstants) <= 128);

/// One overlay draw, recorded inside the window adapt pass's render pass.
struct PerfOverlayDraw {
    VkPipeline pipeline{};
    VkPipelineLayout layout{};
    VkDescriptorSet descriptor_set{};
    PerfOverlayPushConstants push{};
};

void RecordPerfOverlay(vk::CommandBuffer cmdbuf, const PerfOverlayDraw& draw);

/// Draws VideoCore::PerfOverlay::Stats over the presented frame: one quad, one uniform buffer
/// slot per swapchain image, written in place (no allocation per frame).
class PerfOverlayPass {
public:
    explicit PerfOverlayPass(const Device& device, MemoryAllocator& allocator, VkFormat format,
                             size_t image_count);
    ~PerfOverlayPass();

    [[nodiscard]] VkFormat Format() const {
        return format;
    }
    [[nodiscard]] size_t ImageCount() const {
        return image_count;
    }

    /// Writes this frame's text and graph to the slot of image_index.
    [[nodiscard]] PerfOverlayDraw Prepare(size_t image_index, const PerfOverlayInput& input,
                                          const Layout::FramebufferLayout& layout);

private:
    VkFormat format;
    size_t image_count;
    VideoCore::PerfOverlay::Encoding encoding;
    VkDeviceSize slot_size{};
    vk::Buffer buffer;
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSetLayout descriptor_set_layout;
    vk::DescriptorSets descriptor_sets;
    vk::PipelineLayout pipeline_layout;
    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;
    vk::RenderPass render_pass; ///< Compatible with the window adapt pass's; for the pipeline
    vk::Pipeline pipeline;
    std::array<float, VideoCore::PerfOverlay::Stats::GraphSamples> graph{};
};

} // namespace Vulkan
