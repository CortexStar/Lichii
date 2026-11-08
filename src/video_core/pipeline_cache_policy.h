// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

#include "common/common_types.h"

namespace VideoCommon {

/// Attribute mask of a native vertex program, two bits per attribute as in
/// FixedPipelineState::attribute_types.
/// @param input_generic Components of each generic attribute in the program header's input map.
constexpr u64 VertexAttributeMask(std::span<const std::array<bool, 4>> input_generic) {
    u64 mask = 0;
    for (size_t index = 0; index < input_generic.size(); ++index) {
        if (std::ranges::any_of(input_generic[index], [](bool component) { return component; })) {
            mask |= u64{3} << (index * 2);
        }
    }
    return mask;
}

/// Vertex attribute types a graphics pipeline key keeps.
/// @param attribute_types Types of every attribute register.
/// @param unique_hashes   Program hash of each stage, zero when the stage is off.
/// @param attribute_masks ShaderInfo::vertex_attribute_mask of each stage.
/// @return The types masked by every active stage, or all of them when VertexA is on.
constexpr u64 KeyedAttributeTypes(u64 attribute_types, std::span<const u64> unique_hashes,
                                  std::span<const u64> attribute_masks) {
    if (unique_hashes.empty() || unique_hashes[0] != 0) {
        return attribute_types;
    }
    for (size_t stage = 1; stage < unique_hashes.size(); ++stage) {
        if (unique_hashes[stage] != 0) {
            attribute_types &= attribute_masks[stage];
        }
    }
    return attribute_types;
}

/// Attribute registers whose static vertex input a graphics pipeline key keeps, one bit per
/// attribute: those the vertex program reads, or all of them when VertexA is on or the program's
/// reads are unknown.
/// @param unique_hashes   Program hash of each stage, zero when the stage is off.
/// @param attribute_masks ShaderInfo::vertex_attribute_mask of each stage.
constexpr u32 KeyedStaticAttributes(std::span<const u64> unique_hashes,
                                    std::span<const u64> attribute_masks) {
    u64 two_bit_mask = ~u64{0};
    if (!unique_hashes.empty() && unique_hashes[0] == 0) {
        for (size_t stage = 1; stage < unique_hashes.size(); ++stage) {
            if (unique_hashes[stage] != 0) {
                two_bit_mask &= attribute_masks[stage];
            }
        }
    }
    u32 attributes = 0;
    for (u32 index = 0; index < 32; ++index) {
        attributes |= ((two_bit_mask >> (index * 2)) & 3) != 0 ? u32{1} << index : 0;
    }
    return attributes;
}

/**
 * Masks a static vertex input key to what the pipeline uses: the attributes in `keyed` keep their
 * words, the others become zero (the pipeline declares no attribute for them), and only the
 * bindings a kept, enabled attribute fetches from keep their instance divisors.
 * Attribute needs `raw`, `enabled` and `buffer` (FixedPipelineState::VertexAttribute).
 */
template <typename Attribute, size_t NumAttributes, size_t NumBindings>
constexpr void MaskStaticVertexInput(std::array<Attribute, NumAttributes>& attributes,
                                     std::array<u32, NumBindings>& binding_divisors, u32 keyed) {
    u32 used_bindings = 0;
    for (size_t index = 0; index < NumAttributes; ++index) {
        Attribute& attribute = attributes[index];
        if (((keyed >> index) & 1) == 0) {
            attribute.raw = 0;
        } else if (static_cast<u32>(attribute.enabled) != 0) {
            used_bindings |= u32{1} << static_cast<u32>(attribute.buffer);
        }
    }
    for (size_t binding = 0; binding < NumBindings; ++binding) {
        if (((used_bindings >> binding) & 1) == 0) {
            binding_divisors[binding] = 0;
        }
    }
}

} // namespace VideoCommon
