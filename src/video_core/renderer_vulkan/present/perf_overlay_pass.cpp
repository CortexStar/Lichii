// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/alignment.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/host_shaders/vulkan_perf_overlay_frag_spv.h"
#include "video_core/host_shaders/vulkan_perf_overlay_vert_spv.h"
#include "video_core/renderer_vulkan/present/perf_overlay_font.h"
#include "video_core/renderer_vulkan/present/perf_overlay_pass.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

using VideoCore::PerfOverlay::Encoding;
using VideoCore::PerfOverlay::Stats;

// Box layout in font pixels; vulkan_perf_overlay.frag uses the same values.
constexpr u32 Pad = 6;
constexpr u32 RowPitch = 10;
constexpr u32 GraphHeight = 40;
constexpr u32 BoxWidth = Pad * 2 + Stats::Columns * 8;
constexpr u32 BoxHeight = Pad * 2 + Stats::Rows * RowPitch + 2 + GraphHeight;
constexpr u32 TextWords = (Stats::Rows * Stats::Columns + 15) / 16 * 4;

// The shader's std140 block: uvec4 font[64], uvec4 text[], vec4 graph[].
struct OverlayData {
    std::array<u32, 256> font;
    std::array<u32, TextWords> text;
    std::array<f32, Stats::GraphSamples> graph;
};
static_assert(Stats::Columns == 34 && Stats::Rows == 5 && Stats::GraphSamples == 136,
              "update vulkan_perf_overlay.frag");
static_assert(offsetof(OverlayData, text) == 1024 && offsetof(OverlayData, graph) % 16 == 0);
static_assert(sizeof(OverlayData) == offsetof(OverlayData, graph) + Stats::GraphSamples * 4);

constexpr size_t TextOffset = offsetof(OverlayData, text);
constexpr size_t GraphOffset = offsetof(OverlayData, graph);

Encoding EncodingOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return Encoding::Pq;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return Encoding::ScRgb;
    default:
        return Encoding::Srgb;
    }
}

std::array<f32, 4> Color(const std::array<f32, 3>& linear, f32 alpha, f32 white_nits,
                         Encoding encoding) {
    const auto encoded = VideoCore::PerfOverlay::EncodeColor(linear, white_nits, encoding);
    return {encoded[0], encoded[1], encoded[2], alpha};
}

} // Anonymous namespace

void RecordPerfOverlay(vk::CommandBuffer cmdbuf, const PerfOverlayDraw& draw) {
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
    cmdbuf.PushConstants(draw.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         draw.push);
    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, draw.layout, 0, draw.descriptor_set,
                              {});
    cmdbuf.Draw(4, 1, 0, 0);
}

PerfOverlayPass::PerfOverlayPass(const Device& device, MemoryAllocator& allocator, VkFormat format_,
                                 size_t image_count_)
    : format{format_}, image_count{image_count_}, encoding{EncodingOf(format_)} {
    slot_size = Common::AlignUp(sizeof(OverlayData), device.GetUniformBufferAlignment());
    buffer = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = slot_size * image_count,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::Upload);

    // The font never changes: every slot gets it once.
    std::array<u32, 256> font_words{};
    std::memcpy(font_words.data(), PerfOverlayFont.data(), sizeof(font_words));
    const std::span<u8> mapped = buffer.Mapped();
    std::fill(mapped.begin(), mapped.end(), u8{0});
    for (size_t i = 0; i < image_count; ++i) {
        std::memcpy(mapped.data() + i * slot_size, font_words.data(), sizeof(font_words));
    }
    buffer.Flush();

    descriptor_set_layout = CreateWrappedDescriptorSetLayout(
        device, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER}, VK_SHADER_STAGE_FRAGMENT_BIT);
    descriptor_pool = CreateWrappedDescriptorPool(device, image_count, image_count,
                                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER});
    std::vector<VkDescriptorSetLayout> layouts(image_count, *descriptor_set_layout);
    descriptor_sets = CreateWrappedDescriptorSets(descriptor_pool, layouts);
    for (size_t i = 0; i < image_count; ++i) {
        const VkDescriptorBufferInfo buffer_info{
            .buffer = *buffer,
            .offset = i * slot_size,
            .range = sizeof(OverlayData),
        };
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_sets[i],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &buffer_info,
            .pTexelBufferView = nullptr,
        };
        device.GetLogical().UpdateDescriptorSets(std::array{write}, {});
    }

    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PerfOverlayPushConstants),
    };
    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
    vertex_shader = BuildShader(device, VULKAN_PERF_OVERLAY_VERT_SPV);
    fragment_shader = BuildShader(device, VULKAN_PERF_OVERLAY_FRAG_SPV);
    render_pass = CreateWrappedRenderPass(device, format, VK_IMAGE_LAYOUT_UNDEFINED);
    pipeline = CreateWrappedPremultipliedBlendingPipeline(device, render_pass, pipeline_layout,
                                                          std::tie(vertex_shader, fragment_shader));
}

PerfOverlayPass::~PerfOverlayPass() = default;

PerfOverlayDraw PerfOverlayPass::Prepare(size_t image_index, const PerfOverlayInput& input,
                                         const Layout::FramebufferLayout& layout) {
    const Stats& stats = *input.stats;
    u8* const slot = buffer.Mapped().data() + image_index * slot_size;
    const auto text = stats.Text();
    std::memcpy(slot + TextOffset, text.data(), text.size());
    stats.Graph(std::span<float, Stats::GraphSamples>{graph});
    std::memcpy(slot + GraphOffset, graph.data(), sizeof(graph));
    buffer.Flush();

    // Top left of the game's picture, a font pixel per 720 lines of it.
    const u32 screen_height = std::max(layout.screen.GetHeight(), 1u);
    const f32 scale = std::max(1.0f, std::round(static_cast<f32>(screen_height) / 720.0f));
    const f32 x = static_cast<f32>(layout.screen.left) + 8.0f * scale;
    const f32 y = static_cast<f32>(layout.screen.top) + 8.0f * scale;
    const f32 width = static_cast<f32>(std::max(layout.width, 1u));
    const f32 height = static_cast<f32>(std::max(layout.height, 1u));
    const f32 right = x + static_cast<f32>(BoxWidth) * scale;
    const f32 bottom = y + static_cast<f32>(BoxHeight) * scale;

    const f32 white = input.white_nits;
    return PerfOverlayDraw{
        .pipeline = *pipeline,
        .layout = *pipeline_layout,
        .descriptor_set = descriptor_sets[image_index],
        .push =
            {
                .rect = {x / width * 2.0f - 1.0f, y / height * 2.0f - 1.0f,
                         right / width * 2.0f - 1.0f, bottom / height * 2.0f - 1.0f},
                .origin_scale = {x, y, scale, 0.0f},
                .text_color = Color({1.0f, 1.0f, 1.0f}, 1.0f, white, encoding),
                .graph_color = Color({0.35f, 0.85f, 0.35f}, 1.0f, white, encoding),
                .hitch_color = Color({1.0f, 0.3f, 0.2f}, 1.0f, white, encoding),
                .background = {0.0f, 0.0f, 0.0f, 0.6f},
            },
    };
}

} // namespace Vulkan
