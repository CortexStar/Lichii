// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "video_core/renderer_vulkan/dynamic_state_delta.h"

namespace {
using namespace Vulkan::DynamicStateDelta;
namespace vk = Vulkan::vk;

/// Records the vkCmdSet* calls ApplyPacket makes, one string each.
struct FakeCmd {
    mutable std::vector<std::string> calls;

    void Add(std::string call) const {
        calls.push_back(std::move(call));
    }
    void SetViewport(u32 first, vk::Span<VkViewport> v) const {
        Add(fmt::format("viewport {} {} w{}", first, v.size(), v[v.size() - 1].width));
    }
    void SetViewportWithCount(vk::Span<VkViewport> v) const {
        Add(fmt::format("viewport count {}", v.size()));
    }
    void SetScissor(u32 first, vk::Span<VkRect2D> s) const {
        Add(fmt::format("scissor {} {} x{}", first, s.size(), s[0].offset.x));
    }
    void SetScissorWithCount(vk::Span<VkRect2D> s) const {
        Add(fmt::format("scissor count {}", s.size()));
    }
    void SetDepthBias(f32 c, f32 clamp, f32 slope) const {
        Add(fmt::format("bias {} {} {}", c, clamp, slope));
    }
    void SetDepthBias(f32 c, f32, f32, VkDepthBiasRepresentationInfoEXT* info) const {
        Add(fmt::format("bias2 {} {}", c, static_cast<int>(info->depthBiasRepresentation)));
    }
    void SetBlendConstants(const f32 c[4]) const {
        Add(fmt::format("blend constants {} {}", c[0], c[3]));
    }
    void SetDepthBounds(f32 a, f32 b) const {
        Add(fmt::format("bounds {} {}", a, b));
    }
    void SetLineWidth(f32 w) const {
        Add(fmt::format("line width {}", w));
    }
    void SetStencilReference(VkStencilFaceFlags f, u32 v) const {
        Add(fmt::format("ref {} {}", f, v));
    }
    void SetStencilWriteMask(VkStencilFaceFlags f, u32 v) const {
        Add(fmt::format("write mask {} {}", f, v));
    }
    void SetStencilCompareMask(VkStencilFaceFlags f, u32 v) const {
        Add(fmt::format("compare mask {} {}", f, v));
    }
    void SetCullModeEXT(VkCullModeFlags v) const {
        Add(fmt::format("cull {}", v));
    }
    void SetFrontFaceEXT(VkFrontFace v) const {
        Add(fmt::format("front face {}", static_cast<int>(v)));
    }
    void SetDepthTestEnableEXT(bool v) const {
        Add(fmt::format("depth test {}", v));
    }
    void SetDepthWriteEnableEXT(bool v) const {
        Add(fmt::format("depth write {}", v));
    }
    void SetDepthCompareOpEXT(VkCompareOp v) const {
        Add(fmt::format("depth compare {}", static_cast<int>(v)));
    }
    void SetDepthBoundsTestEnableEXT(bool v) const {
        Add(fmt::format("bounds test {}", v));
    }
    void SetStencilTestEnableEXT(bool v) const {
        Add(fmt::format("stencil test {}", v));
    }
    void SetStencilOpEXT(VkStencilFaceFlags f, VkStencilOp fail, VkStencilOp pass,
                         VkStencilOp depth_fail, VkCompareOp compare) const {
        Add(fmt::format("stencil op {} {} {} {} {}", f, static_cast<int>(fail),
                        static_cast<int>(pass), static_cast<int>(depth_fail),
                        static_cast<int>(compare)));
    }
    void SetPrimitiveTopologyEXT(VkPrimitiveTopology v) const {
        Add(fmt::format("topology {}", static_cast<int>(v)));
    }
    void SetPrimitiveRestartEnableEXT(bool v) const {
        Add(fmt::format("restart {}", v));
    }
    void SetRasterizerDiscardEnableEXT(bool v) const {
        Add(fmt::format("discard {}", v));
    }
    void SetDepthBiasEnableEXT(bool v) const {
        Add(fmt::format("bias enable {}", v));
    }
    void SetLogicOpEXT(VkLogicOp v) const {
        Add(fmt::format("logic op {}", static_cast<int>(v)));
    }
    void SetLogicOpEnableEXT(bool v) const {
        Add(fmt::format("logic op enable {}", v));
    }
    void SetDepthClampEnableEXT(bool v) const {
        Add(fmt::format("clamp {}", v));
    }
    void SetLineRasterizationModeEXT(VkLineRasterizationModeEXT v) const {
        Add(fmt::format("line mode {}", static_cast<int>(v)));
    }
    void SetLineStippleEnableEXT(bool v) const {
        Add(fmt::format("stipple enable {}", v));
    }
    void SetConservativeRasterizationModeEXT(VkConservativeRasterizationModeEXT v) const {
        Add(fmt::format("conservative {}", static_cast<int>(v)));
    }
    void SetAlphaToCoverageEnableEXT(bool v) const {
        Add(fmt::format("a2c {}", v));
    }
    void SetAlphaToOneEnableEXT(bool v) const {
        Add(fmt::format("a2one {}", v));
    }
    void SetColorWriteMaskEXT(u32 first, vk::Span<VkColorComponentFlags> m) const {
        Add(fmt::format("write masks {} {} {}", first, m[0], m[7]));
    }
    void SetColorBlendEnableEXT(u32 first, vk::Span<VkBool32> e) const {
        Add(fmt::format("blend enables {} {} {}", first, e[0], e[1]));
    }
    void SetColorBlendEquationEXT(u32 first, vk::Span<VkColorBlendEquationEXT> e) const {
        Add(fmt::format("blend equations {} {} {}", first,
                        static_cast<int>(e[1].dstColorBlendFactor),
                        static_cast<int>(e[1].alphaBlendOp)));
    }
    void SetColorWriteEnableEXT(vk::Span<VkBool32> e) const {
        Add(fmt::format("write enables {} {}", e[0], e[2]));
    }
    void SetVertexInputEXT(vk::Span<VkVertexInputBindingDescription2EXT> b,
                           vk::Span<VkVertexInputAttributeDescription2EXT> a) const {
        Add(fmt::format("vertex input {} {}", b.size(), a.size()));
    }
    void SetPolygonModeEXT(VkPolygonMode v) const {
        Add(fmt::format("polygon {}", static_cast<int>(v)));
    }
    void SetRasterizationSamplesEXT(VkSampleCountFlagBits v) const {
        Add(fmt::format("samples {}", static_cast<int>(v)));
    }
    void SetSampleMaskEXT(VkSampleCountFlagBits s, u32 m) const {
        Add(fmt::format("sample mask {} {}", static_cast<int>(s), m));
    }
    void SetTessellationDomainOriginEXT(VkTessellationDomainOrigin v) const {
        Add(fmt::format("domain origin {}", static_cast<int>(v)));
    }
    void SetPatchControlPointsEXT(u32 v) const {
        Add(fmt::format("patch points {}", v));
    }
    void SetDepthClipEnableEXT(bool v) const {
        Add(fmt::format("depth clip {}", v));
    }
    void SetDepthClipNegativeOneToOneEXT(bool v) const {
        Add(fmt::format("ndc {}", v));
    }
    void SetProvokingVertexModeEXT(VkProvokingVertexModeEXT v) const {
        Add(fmt::format("provoking {}", static_cast<int>(v)));
    }
    void SetLineStippleEXT(u32 factor, u16 pattern) const {
        Add(fmt::format("stipple {} {}", factor, pattern));
    }
};

std::vector<std::string> Apply(Tracker& tracker, const ApplyOptions& options = {}) {
    std::array<u32, MAX_PACKET_WORDS> packet;
    const size_t size = tracker.Take(packet);
    FakeCmd cmd;
    if (size != 0) {
        ApplyPacket(std::span<const u32>(packet.data(), size), cmd, options);
    }
    return cmd.calls;
}

std::array<u32, 1 + 2 * 6> TwoViewports(f32 width) {
    std::array<u32, 1 + 2 * 6> words{2};
    for (u32 i = 0; i < 2; ++i) {
        words[1 + i * 6 + 2] = FloatWord(width + static_cast<f32>(i));
    }
    return words;
}
} // Anonymous namespace

TEST_CASE("A staged value the command buffer holds is not recorded again", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    REQUIRE(tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_BACK_BIT}));
    REQUIRE(Apply(tracker) == std::vector<std::string>{"cull 2"});
    REQUIRE_FALSE(tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_BACK_BIT}));
    REQUIRE(tracker.Pending() == 0);
    REQUIRE(Apply(tracker).empty());
    REQUIRE(tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_FRONT_BIT}));
    REQUIRE(Apply(tracker) == std::vector<std::string>{"cull 1"});
}

TEST_CASE("A new command buffer holds no dynamic state", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    REQUIRE(tracker.Stage(Slot::DepthTestEnable, u32{1}));
    (void)Apply(tracker);
    tracker.SetEpoch(1);
    REQUIRE_FALSE(tracker.Stage(Slot::DepthTestEnable, u32{1}));
    tracker.SetEpoch(2);
    REQUIRE(tracker.Held(Slot::DepthTestEnable).empty());
    REQUIRE(tracker.Stage(Slot::DepthTestEnable, u32{1}));
}

TEST_CASE("Forgotten slots are recorded again, the others are kept", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::PrimitiveTopology, u32{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST});
    (void)tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_BACK_BIT});
    (void)Apply(tracker);
    tracker.Forget(Bit(Slot::PrimitiveTopology));
    REQUIRE(tracker.Stage(Slot::PrimitiveTopology, u32{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST}));
    REQUIRE_FALSE(tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_BACK_BIT}));
}

TEST_CASE("A packet holds the changed slots in slot order", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::DepthCompareOp, u32{VK_COMPARE_OP_LESS});
    (void)tracker.Stage(Slot::Viewports, TwoViewports(100.0f));
    (void)tracker.Stage(Slot::LineWidth, FloatWord(2.0f));
    std::array<u32, MAX_PACKET_WORDS> packet;
    const size_t size = tracker.Take(packet);
    const u64 mask = Bit(Slot::Viewports) | Bit(Slot::LineWidth) | Bit(Slot::DepthCompareOp);
    REQUIRE(size == 2 + 13 + 1 + 1);
    REQUIRE(packet[0] == static_cast<u32>(mask));
    REQUIRE(packet[1] == static_cast<u32>(mask >> 32));
    REQUIRE(packet[2] == 2);                   // viewport count
    REQUIRE(packet[15] == FloatWord(2.0f));    // line width
    REQUIRE(packet[16] == VK_COMPARE_OP_LESS); // depth compare op
}

TEST_CASE("A viewport count change is a change", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::Viewports, TwoViewports(100.0f));
    (void)Apply(tracker);
    std::array<u32, 1 + 6> one{1};
    one[3] = FloatWord(100.0f);
    REQUIRE(tracker.Stage(Slot::Viewports, one));
    REQUIRE(Apply(tracker) == std::vector<std::string>{"viewport 0 1 w100"});
    REQUIRE(tracker.Stage(Slot::Viewports, TwoViewports(100.0f)));
    REQUIRE(Apply(tracker, {.with_count = true}) == std::vector<std::string>{"viewport count 2"});
}

TEST_CASE("The generation moves only when a draw's dynamic state changes", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_BACK_BIT});
    (void)Apply(tracker);
    const u64 after_first = tracker.Generation();
    (void)tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_BACK_BIT});
    (void)Apply(tracker);
    REQUIRE(tracker.Generation() == after_first);
    (void)tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_NONE});
    (void)Apply(tracker);
    REQUIRE(tracker.Generation() != after_first);
    const u64 before_epoch = tracker.Generation();
    tracker.SetEpoch(2);
    REQUIRE(tracker.Generation() != before_epoch);
}

TEST_CASE("Taking some slots leaves the others staged", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::CullMode, u32{VK_CULL_MODE_BACK_BIT});
    (void)tracker.Stage(Slot::FrontFace, u32{VK_FRONT_FACE_CLOCKWISE});
    std::array<u32, MAX_PACKET_WORDS> packet;
    REQUIRE(tracker.TakeSlots(Bit(Slot::FrontFace), packet) == 3);
    REQUIRE(tracker.Pending() == Bit(Slot::CullMode));
    REQUIRE(Apply(tracker) == std::vector<std::string>{"cull 2"});
}

TEST_CASE("Stencil values with equal faces are recorded for both faces at once",
          "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::StencilReference, std::array<u32, 2>{5, 5});
    (void)tracker.Stage(Slot::StencilWriteMask, std::array<u32, 2>{0xFF, 0x0F});
    REQUIRE(Apply(tracker) ==
            std::vector<std::string>{"ref 3 5", "write mask 1 255", "write mask 2 15"});
    const std::array<u32, 9> one_sided{0, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE,
                                       VK_STENCIL_OP_ZERO, VK_COMPARE_OP_ALWAYS};
    (void)tracker.Stage(Slot::StencilOp, one_sided);
    REQUIRE(Apply(tracker) == std::vector<std::string>{"stencil op 3 0 2 1 7"});
    std::array<u32, 9> two_sided = one_sided;
    two_sided[0] = 1;
    two_sided[8] = VK_COMPARE_OP_NEVER;
    (void)tracker.Stage(Slot::StencilOp, two_sided);
    REQUIRE(Apply(tracker) ==
            std::vector<std::string>{"stencil op 1 0 2 1 7", "stencil op 2 0 0 0 0"});
}

TEST_CASE("Blend equations round-trip through their packed word", "[dynamic_state]") {
    const VkColorBlendEquationEXT eq{
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC1_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
        .colorBlendOp = VK_BLEND_OP_MAX,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT,
    };
    const VkColorBlendEquationEXT back = UnpackBlendEquation(PackBlendEquation(eq));
    REQUIRE(back.srcColorBlendFactor == eq.srcColorBlendFactor);
    REQUIRE(back.dstColorBlendFactor == eq.dstColorBlendFactor);
    REQUIRE(back.colorBlendOp == eq.colorBlendOp);
    REQUIRE(back.srcAlphaBlendFactor == eq.srcAlphaBlendFactor);
    REQUIRE(back.dstAlphaBlendFactor == eq.dstAlphaBlendFactor);
    REQUIRE(back.alphaBlendOp == eq.alphaBlendOp);
}

TEST_CASE("Color slots expand to one entry per render target", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::ColorWriteMask, u32{0xA000000F});
    (void)tracker.Stage(Slot::ColorBlendEnable, u32{0b10});
    std::array<u32, NUM_RENDER_TARGETS> equations{};
    equations[1] =
        PackBlendEquation({VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_OP_ADD,
                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_MIN});
    (void)tracker.Stage(Slot::ColorBlendEquation, equations);
    REQUIRE(Apply(tracker) == std::vector<std::string>{"write masks 0 15 10", "blend enables 0 0 1",
                                                       "blend equations 0 8 3"});
}

TEST_CASE("Vertex input layouts are recorded by pointer", "[dynamic_state]") {
    VertexInputLayout layout;
    layout.bindings.resize(3);
    layout.attributes.resize(5);
    Tracker tracker;
    tracker.SetEpoch(1);
    REQUIRE(tracker.Stage(Slot::VertexInput, VertexInputWords(&layout)));
    REQUIRE_FALSE(tracker.Stage(Slot::VertexInput, VertexInputWords(&layout)));
    REQUIRE(Apply(tracker) == std::vector<std::string>{"vertex input 3 5"});
}

TEST_CASE("Depth bias uses the representation the device takes", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    const std::array<u32, 3> bias{FloatWord(1.0f), FloatWord(0.0f), FloatWord(2.0f)};
    (void)tracker.Stage(Slot::DepthBias, bias);
    REQUIRE(Apply(tracker) == std::vector<std::string>{"bias 1 0 2"});
    tracker.SetEpoch(2);
    (void)tracker.Stage(Slot::DepthBias, bias);
    REQUIRE(Apply(tracker, {.depth_bias_force_unorm = true}) ==
            std::vector<std::string>{fmt::format(
                "bias2 1 {}",
                static_cast<int>(
                    VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT))});
}

TEST_CASE("Shader object state slots record their calls", "[dynamic_state]") {
    Tracker tracker;
    tracker.SetEpoch(1);
    (void)tracker.Stage(Slot::PolygonMode, u32{VK_POLYGON_MODE_LINE});
    (void)tracker.Stage(Slot::SampleMask, std::array<u32, 2>{VK_SAMPLE_COUNT_4_BIT, 0xF});
    (void)tracker.Stage(Slot::LineStipple, std::array<u32, 2>{2, 0xF0F0});
    REQUIRE(Apply(tracker) ==
            std::vector<std::string>{"polygon 1", "sample mask 4 15", "stipple 2 61680"});
}
