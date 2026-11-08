// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <type_traits>

#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

// Dynamic-state deltas (notes/native-gpu-backend.md step 6). Every dynamic state a draw needs is
// one Slot holding a few words in the form the vkCmdSet* call takes. The rasterizer stages the
// value of each slot whose registers changed; a slot whose value equals what the command buffer
// already holds is dropped, and the rest go into one packet per draw: [mask lo, mask hi, the
// changed slots' words in slot order]. ApplyDynamicStatePacket records a packet's vkCmdSet*
// calls on the recording thread.
namespace Vulkan::DynamicStateDelta {

enum class Slot : u32 {
    Viewports, ///< count, then per viewport x, y, width, height, min depth, max depth
    Scissors,  ///< count, then per scissor x, y, width, height
    DepthBias, ///< constant, clamp, slope (f32)
    BlendConstants,
    DepthBounds,
    LineWidth,
    StencilReference,   ///< front, back (back = front when one-sided)
    StencilWriteMask,   ///< front, back
    StencilCompareMask, ///< front, back
    CullMode,
    FrontFace,
    DepthTestEnable,
    DepthWriteEnable,
    DepthCompareOp,
    DepthBoundsTestEnable,
    StencilTestEnable,
    StencilOp, ///< two-sided, front fail/pass/depth fail/compare, back fail/pass/depth fail/compare
    PrimitiveTopology,
    PrimitiveRestartEnable,
    RasterizerDiscardEnable,
    DepthBiasEnable,
    LogicOp,
    LogicOpEnable,
    DepthClampEnable,
    LineRasterizationMode,
    LineStippleEnable,
    ConservativeRasterizationMode,
    AlphaToCoverageEnable,
    AlphaToOneEnable,
    ColorWriteMask,     ///< 4 bits per render target
    ColorBlendEnable,   ///< 1 bit per render target
    ColorBlendEquation, ///< per render target PackBlendEquation
    ColorWriteEnable,   ///< 1 bit per render target (VK_EXT_color_write_enable)
    VertexInput,        ///< const VertexInputLayout*, low and high word
    // State a pipeline holds statically, dynamic with shader objects.
    PolygonMode,
    RasterizationSamples,
    SampleMask, ///< samples (VkSampleCountFlagBits), mask
    TessellationDomainOrigin,
    PatchControlPoints,
    DepthClipEnable,
    DepthClipNegativeOneToOne,
    ProvokingVertexMode,
    LineStipple, ///< factor, pattern
    Count,
};

inline constexpr size_t NUM_SLOTS = static_cast<size_t>(Slot::Count);
static_assert(NUM_SLOTS <= 64);
inline constexpr u32 MAX_VIEWPORTS = 16;
inline constexpr u32 NUM_RENDER_TARGETS = 8;

[[nodiscard]] constexpr u64 Bit(Slot slot) noexcept {
    return u64{1} << static_cast<u32>(slot);
}

/// The most words a slot's value takes.
[[nodiscard]] constexpr u32 MaxWords(Slot slot) noexcept {
    switch (slot) {
    case Slot::Viewports:
        return 1 + MAX_VIEWPORTS * 6;
    case Slot::Scissors:
        return 1 + MAX_VIEWPORTS * 4;
    case Slot::DepthBias:
        return 3;
    case Slot::BlendConstants:
        return 4;
    case Slot::DepthBounds:
    case Slot::SampleMask:
    case Slot::StencilReference:
    case Slot::StencilWriteMask:
    case Slot::StencilCompareMask:
    case Slot::VertexInput:
    case Slot::LineStipple:
        return 2;
    case Slot::StencilOp:
        return 9;
    case Slot::ColorBlendEquation:
        return NUM_RENDER_TARGETS;
    default:
        return 1;
    }
}

/// Viewports and scissors carry their count in the first word.
[[nodiscard]] constexpr u32 WordsOf(Slot slot, const u32* words) noexcept {
    switch (slot) {
    case Slot::Viewports:
        return 1 + words[0] * 6;
    case Slot::Scissors:
        return 1 + words[0] * 4;
    default:
        return MaxWords(slot);
    }
}

inline constexpr std::array<u32, NUM_SLOTS + 1> SLOT_OFFSETS = [] {
    std::array<u32, NUM_SLOTS + 1> offsets{};
    for (size_t slot = 0; slot < NUM_SLOTS; ++slot) {
        offsets[slot + 1] = offsets[slot] + MaxWords(static_cast<Slot>(slot));
    }
    return offsets;
}();
inline constexpr u32 TOTAL_WORDS = SLOT_OFFSETS[NUM_SLOTS];
/// The largest packet: the mask and every slot at its largest.
inline constexpr u32 MAX_PACKET_WORDS = 2 + TOTAL_WORDS;

/// VkColorBlendEquationEXT in one word: the factors take 5 bits, the ops 3 (VkBlendOp values
/// up to VK_BLEND_OP_MAX; the advanced ops are not used here).
[[nodiscard]] constexpr u32 PackBlendEquation(const VkColorBlendEquationEXT& eq) noexcept {
    return static_cast<u32>(eq.srcColorBlendFactor) |
           static_cast<u32>(eq.dstColorBlendFactor) << 5 | static_cast<u32>(eq.colorBlendOp) << 10 |
           static_cast<u32>(eq.srcAlphaBlendFactor) << 13 |
           static_cast<u32>(eq.dstAlphaBlendFactor) << 18 | static_cast<u32>(eq.alphaBlendOp) << 23;
}

[[nodiscard]] constexpr VkColorBlendEquationEXT UnpackBlendEquation(u32 word) noexcept {
    return VkColorBlendEquationEXT{
        .srcColorBlendFactor = static_cast<VkBlendFactor>(word & 0x1F),
        .dstColorBlendFactor = static_cast<VkBlendFactor>((word >> 5) & 0x1F),
        .colorBlendOp = static_cast<VkBlendOp>((word >> 10) & 0x7),
        .srcAlphaBlendFactor = static_cast<VkBlendFactor>((word >> 13) & 0x1F),
        .dstAlphaBlendFactor = static_cast<VkBlendFactor>((word >> 18) & 0x1F),
        .alphaBlendOp = static_cast<VkBlendOp>((word >> 23) & 0x7),
    };
}

[[nodiscard]] inline u32 FloatWord(f32 value) noexcept {
    return std::bit_cast<u32>(value);
}

[[nodiscard]] inline f32 WordFloat(u32 word) noexcept {
    return std::bit_cast<f32>(word);
}

/// A dynamic vertex input layout, recorded by pointer (the owner keeps it alive while recorded
/// command buffers may use it).
struct VertexInputLayout {
    boost::container::static_vector<VkVertexInputBindingDescription2EXT, 32> bindings;
    boost::container::static_vector<VkVertexInputAttributeDescription2EXT, 32> attributes;
};

/// The values the command buffer holds per slot, and the slots staged since the last packet.
class Tracker {
public:
    /// Values recorded into an earlier command buffer (or before another pipeline's static
    /// state replaced them) are not held any more.
    void SetEpoch(u64 epoch) noexcept {
        if (epoch != current_epoch) {
            current_epoch = epoch;
            valid = 0;
            ++generation;
        }
    }

    /// The command buffer may no longer hold these slots' values (called between packets).
    void Forget(u64 slots) noexcept {
        valid &= ~slots;
        pending &= ~slots;
    }

    /// Stages `words` for the slot unless the command buffer holds that value already.
    /// True when staged.
    bool Stage(Slot slot, std::span<const u32> words) noexcept {
        const u32 index = static_cast<u32>(slot);
        ASSERT(words.size() <= MaxWords(slot) && !words.empty());
        u32* const held = values.data() + SLOT_OFFSETS[index];
        if (Holds(slot, words)) {
            return false;
        }
        std::memcpy(held, words.data(), words.size_bytes());
        valid |= Bit(slot);
        pending |= Bit(slot);
        return true;
    }

    template <typename T>
        requires((std::is_arithmetic_v<T> || std::is_enum_v<T>) && sizeof(T) == sizeof(u32))
    bool Stage(Slot slot, T value) noexcept {
        u32 word;
        std::memcpy(&word, &value, sizeof(word));
        return Stage(slot, std::span<const u32>(&word, 1));
    }

    /// Whether the command buffer (with the staged slots) holds `words` for the slot.
    [[nodiscard]] bool Holds(Slot slot, std::span<const u32> words) const noexcept {
        if ((valid & Bit(slot)) == 0) {
            return false;
        }
        const u32* const held = values.data() + SLOT_OFFSETS[static_cast<u32>(slot)];
        return WordsOf(slot, held) == words.size() &&
               std::memcmp(held, words.data(), words.size_bytes()) == 0;
    }

    /// The value the command buffer holds (with the staged slots); empty when it holds none.
    [[nodiscard]] std::span<const u32> Held(Slot slot) const noexcept {
        if ((valid & Bit(slot)) == 0) {
            return {};
        }
        const u32* const held = values.data() + SLOT_OFFSETS[static_cast<u32>(slot)];
        return {held, WordsOf(slot, held)};
    }

    [[nodiscard]] u64 Pending() const noexcept {
        return pending;
    }

    /// Writes the staged slots as one packet and clears them; returns its words (0: nothing
    /// staged). `out` holds at least MAX_PACKET_WORDS.
    size_t Take(std::span<u32> out) noexcept {
        return TakeSlots(pending, out);
    }

    /// Take for the given staged slots only.
    size_t TakeSlots(u64 slots, std::span<u32> out) noexcept {
        slots &= pending;
        if (slots == 0) {
            return 0;
        }
        out[0] = static_cast<u32>(slots);
        out[1] = static_cast<u32>(slots >> 32);
        size_t size = 2;
        for (u64 rest = slots; rest != 0; rest &= rest - 1) {
            const u32 index = static_cast<u32>(std::countr_zero(rest));
            const u32* const held = values.data() + SLOT_OFFSETS[index];
            const u32 count = WordsOf(static_cast<Slot>(index), held);
            std::copy_n(held, count, out.data() + size);
            size += count;
        }
        pending &= ~slots;
        ++generation;
        return size;
    }

    /// Changes whenever a draw's dynamic state differs from the previous draw's: a packet was
    /// taken or the command buffer's state was reset. Draws between which it is unchanged
    /// share every dynamic state (what a device-generated-commands run needs).
    [[nodiscard]] u64 Generation() const noexcept {
        return generation;
    }

private:
    std::array<u32, TOTAL_WORDS> values{};
    u64 valid{};
    u64 pending{};
    u64 current_epoch{};
    u64 generation{1};
};

/// How a packet is recorded: what the device and the bound shaders take.
struct ApplyOptions {
    /// Shader objects: viewport and scissor counts are dynamic too.
    bool with_count{};
    /// VK_EXT_depth_bias_control: the bias is in the depth format's least representable unit
    /// as if it were UNORM.
    bool depth_bias_force_unorm{};
};

namespace Detail {
inline VkStencilFaceFlags StencilFaces(u32 front, u32 back) noexcept {
    return front == back ? VK_STENCIL_FACE_FRONT_AND_BACK : VK_STENCIL_FACE_FRONT_BIT;
}
} // namespace Detail

/// Records a packet's vkCmdSet* calls. `Cmd` is vk::CommandBuffer (or a test double with its
/// member functions).
template <typename Cmd>
void ApplyPacket(std::span<const u32> packet, const Cmd& cmd, const ApplyOptions& options) {
    const u64 mask = u64{packet[0]} | u64{packet[1]} << 32;
    const u32* w = packet.data() + 2;
    for (u64 rest = mask; rest != 0; rest &= rest - 1) {
        const auto slot = static_cast<Slot>(std::countr_zero(rest));
        switch (slot) {
        case Slot::Viewports: {
            std::array<VkViewport, MAX_VIEWPORTS> viewports;
            const u32 count = w[0];
            for (u32 i = 0; i < count; ++i) {
                const u32* const v = w + 1 + i * 6;
                viewports[i] = {WordFloat(v[0]), WordFloat(v[1]), WordFloat(v[2]),
                                WordFloat(v[3]), WordFloat(v[4]), WordFloat(v[5])};
            }
            if (options.with_count) {
                cmd.SetViewportWithCount(vk::Span<VkViewport>(viewports.data(), count));
            } else {
                cmd.SetViewport(0, vk::Span<VkViewport>(viewports.data(), count));
            }
            break;
        }
        case Slot::Scissors: {
            std::array<VkRect2D, MAX_VIEWPORTS> scissors;
            const u32 count = w[0];
            for (u32 i = 0; i < count; ++i) {
                const u32* const s = w + 1 + i * 4;
                scissors[i] = {{static_cast<s32>(s[0]), static_cast<s32>(s[1])}, {s[2], s[3]}};
            }
            if (options.with_count) {
                cmd.SetScissorWithCount(vk::Span<VkRect2D>(scissors.data(), count));
            } else {
                cmd.SetScissor(0, vk::Span<VkRect2D>(scissors.data(), count));
            }
            break;
        }
        case Slot::DepthBias:
            if (options.depth_bias_force_unorm) {
                VkDepthBiasRepresentationInfoEXT info{
                    .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
                    .pNext = nullptr,
                    .depthBiasRepresentation =
                        VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT,
                    .depthBiasExact = VK_FALSE,
                };
                cmd.SetDepthBias(WordFloat(w[0]), WordFloat(w[1]), WordFloat(w[2]), &info);
            } else {
                cmd.SetDepthBias(WordFloat(w[0]), WordFloat(w[1]), WordFloat(w[2]));
            }
            break;
        case Slot::BlendConstants: {
            const std::array<f32, 4> constants{WordFloat(w[0]), WordFloat(w[1]), WordFloat(w[2]),
                                               WordFloat(w[3])};
            cmd.SetBlendConstants(constants.data());
            break;
        }
        case Slot::DepthBounds:
            cmd.SetDepthBounds(WordFloat(w[0]), WordFloat(w[1]));
            break;
        case Slot::LineWidth:
            cmd.SetLineWidth(WordFloat(w[0]));
            break;
        case Slot::StencilReference:
            cmd.SetStencilReference(Detail::StencilFaces(w[0], w[1]), w[0]);
            if (w[0] != w[1]) {
                cmd.SetStencilReference(VK_STENCIL_FACE_BACK_BIT, w[1]);
            }
            break;
        case Slot::StencilWriteMask:
            cmd.SetStencilWriteMask(Detail::StencilFaces(w[0], w[1]), w[0]);
            if (w[0] != w[1]) {
                cmd.SetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT, w[1]);
            }
            break;
        case Slot::StencilCompareMask:
            cmd.SetStencilCompareMask(Detail::StencilFaces(w[0], w[1]), w[0]);
            if (w[0] != w[1]) {
                cmd.SetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT, w[1]);
            }
            break;
        case Slot::CullMode:
            cmd.SetCullModeEXT(static_cast<VkCullModeFlags>(w[0]));
            break;
        case Slot::FrontFace:
            cmd.SetFrontFaceEXT(static_cast<VkFrontFace>(w[0]));
            break;
        case Slot::DepthTestEnable:
            cmd.SetDepthTestEnableEXT(w[0] != 0);
            break;
        case Slot::DepthWriteEnable:
            cmd.SetDepthWriteEnableEXT(w[0] != 0);
            break;
        case Slot::DepthCompareOp:
            cmd.SetDepthCompareOpEXT(static_cast<VkCompareOp>(w[0]));
            break;
        case Slot::DepthBoundsTestEnable:
            cmd.SetDepthBoundsTestEnableEXT(w[0] != 0);
            break;
        case Slot::StencilTestEnable:
            cmd.SetStencilTestEnableEXT(w[0] != 0);
            break;
        case Slot::StencilOp: {
            const auto op = [](u32 word) { return static_cast<VkStencilOp>(word); };
            const VkStencilFaceFlags front_faces =
                w[0] != 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_FRONT_AND_BACK;
            cmd.SetStencilOpEXT(front_faces, op(w[1]), op(w[2]), op(w[3]),
                                static_cast<VkCompareOp>(w[4]));
            if (w[0] != 0) {
                cmd.SetStencilOpEXT(VK_STENCIL_FACE_BACK_BIT, op(w[5]), op(w[6]), op(w[7]),
                                    static_cast<VkCompareOp>(w[8]));
            }
            break;
        }
        case Slot::PrimitiveTopology:
            cmd.SetPrimitiveTopologyEXT(static_cast<VkPrimitiveTopology>(w[0]));
            break;
        case Slot::PrimitiveRestartEnable:
            cmd.SetPrimitiveRestartEnableEXT(w[0] != 0);
            break;
        case Slot::RasterizerDiscardEnable:
            cmd.SetRasterizerDiscardEnableEXT(w[0] != 0);
            break;
        case Slot::DepthBiasEnable:
            cmd.SetDepthBiasEnableEXT(w[0] != 0);
            break;
        case Slot::LogicOp:
            cmd.SetLogicOpEXT(static_cast<VkLogicOp>(w[0]));
            break;
        case Slot::LogicOpEnable:
            cmd.SetLogicOpEnableEXT(w[0] != 0);
            break;
        case Slot::DepthClampEnable:
            cmd.SetDepthClampEnableEXT(w[0] != 0);
            break;
        case Slot::LineRasterizationMode:
            cmd.SetLineRasterizationModeEXT(static_cast<VkLineRasterizationModeEXT>(w[0]));
            break;
        case Slot::LineStippleEnable:
            cmd.SetLineStippleEnableEXT(w[0] != 0);
            break;
        case Slot::ConservativeRasterizationMode:
            cmd.SetConservativeRasterizationModeEXT(
                static_cast<VkConservativeRasterizationModeEXT>(w[0]));
            break;
        case Slot::AlphaToCoverageEnable:
            cmd.SetAlphaToCoverageEnableEXT(w[0] != 0);
            break;
        case Slot::AlphaToOneEnable:
            cmd.SetAlphaToOneEnableEXT(w[0] != 0);
            break;
        case Slot::ColorWriteMask: {
            std::array<VkColorComponentFlags, NUM_RENDER_TARGETS> masks;
            for (u32 rt = 0; rt < NUM_RENDER_TARGETS; ++rt) {
                masks[rt] = (w[0] >> (rt * 4)) & 0xF;
            }
            cmd.SetColorWriteMaskEXT(0, masks);
            break;
        }
        case Slot::ColorBlendEnable: {
            std::array<VkBool32, NUM_RENDER_TARGETS> enables;
            for (u32 rt = 0; rt < NUM_RENDER_TARGETS; ++rt) {
                enables[rt] = (w[0] >> rt) & 1;
            }
            cmd.SetColorBlendEnableEXT(0, enables);
            break;
        }
        case Slot::ColorBlendEquation: {
            std::array<VkColorBlendEquationEXT, NUM_RENDER_TARGETS> equations;
            for (u32 rt = 0; rt < NUM_RENDER_TARGETS; ++rt) {
                equations[rt] = UnpackBlendEquation(w[rt]);
            }
            cmd.SetColorBlendEquationEXT(0, equations);
            break;
        }
        case Slot::ColorWriteEnable: {
            std::array<VkBool32, NUM_RENDER_TARGETS> enables;
            for (u32 rt = 0; rt < NUM_RENDER_TARGETS; ++rt) {
                enables[rt] = (w[0] >> rt) & 1;
            }
            cmd.SetColorWriteEnableEXT(enables);
            break;
        }
        case Slot::VertexInput: {
            const uintptr_t address = uintptr_t{w[0]} | static_cast<uintptr_t>(u64{w[1]} << 32);
            const auto* const layout = reinterpret_cast<const VertexInputLayout*>(address);
            cmd.SetVertexInputEXT(layout->bindings, layout->attributes);
            break;
        }
        case Slot::PolygonMode:
            cmd.SetPolygonModeEXT(static_cast<VkPolygonMode>(w[0]));
            break;
        case Slot::RasterizationSamples:
            cmd.SetRasterizationSamplesEXT(static_cast<VkSampleCountFlagBits>(w[0]));
            break;
        case Slot::SampleMask:
            cmd.SetSampleMaskEXT(static_cast<VkSampleCountFlagBits>(w[0]), w[1]);
            break;
        case Slot::TessellationDomainOrigin:
            cmd.SetTessellationDomainOriginEXT(static_cast<VkTessellationDomainOrigin>(w[0]));
            break;
        case Slot::PatchControlPoints:
            cmd.SetPatchControlPointsEXT(w[0]);
            break;
        case Slot::DepthClipEnable:
            cmd.SetDepthClipEnableEXT(w[0] != 0);
            break;
        case Slot::DepthClipNegativeOneToOne:
            cmd.SetDepthClipNegativeOneToOneEXT(w[0] != 0);
            break;
        case Slot::ProvokingVertexMode:
            cmd.SetProvokingVertexModeEXT(static_cast<VkProvokingVertexModeEXT>(w[0]));
            break;
        case Slot::LineStipple:
            cmd.SetLineStippleEXT(w[0], static_cast<u16>(w[1]));
            break;
        case Slot::Count:
            UNREACHABLE();
        }
        w += WordsOf(slot, w);
    }
}

/// The words of a VertexInput slot.
[[nodiscard]] inline std::array<u32, 2> VertexInputWords(const VertexInputLayout* layout) noexcept {
    const auto address = static_cast<u64>(reinterpret_cast<uintptr_t>(layout));
    return {static_cast<u32>(address), static_cast<u32>(address >> 32)};
}

} // namespace Vulkan::DynamicStateDelta
