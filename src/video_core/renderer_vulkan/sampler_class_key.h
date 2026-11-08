// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace Vulkan {

/// What a host sampler variant takes from the view it samples (Sampler::MakeKey). Views with
/// equal traits get the same host sampler from one guest TSC: one sampler class of the
/// pool-mirroring descriptor heap.
struct SamplerViewTraits {
    bool anisotropy{};    ///< ImageViewBase::SupportsAnisotropy
    bool integer{};       ///< Integer format: nearest filtering
    bool minmax{};        ///< SupportsMinmaxFilter
    bool border_format{}; ///< RequiresBorderColorFormat
    bool srgb{};          ///< sRGB format: sRGB border color
    bool identity_swizzle{};
    std::array<u8, 4> swizzle{}; ///< VkComponentSwizzle values when not identity

    bool operator==(const SamplerViewTraits&) const = default;
};

namespace SamplerClassKeyBits {
constexpr u64 ANISOTROPY = 1ULL << 0;
constexpr u64 INTEGER = 1ULL << 1;
constexpr u64 MINMAX = 1ULL << 2;
constexpr u64 BORDER_FORMAT = 1ULL << 3;
constexpr u64 SRGB = 1ULL << 4;
constexpr u64 IDENTITY_SWIZZLE = 1ULL << 5;
constexpr u32 SWIZZLE_SHIFT = 8;
} // namespace SamplerClassKeyBits

[[nodiscard]] constexpr u64 PackSamplerClassKey(const SamplerViewTraits& traits) noexcept {
    namespace Bits = SamplerClassKeyBits;
    u64 key = 0;
    key |= traits.anisotropy ? Bits::ANISOTROPY : 0;
    key |= traits.integer ? Bits::INTEGER : 0;
    key |= traits.minmax ? Bits::MINMAX : 0;
    key |= traits.border_format ? Bits::BORDER_FORMAT : 0;
    key |= traits.srgb ? Bits::SRGB : 0;
    if (traits.identity_swizzle) {
        // The components do not matter then: one class for every identity view.
        return key | Bits::IDENTITY_SWIZZLE;
    }
    for (size_t i = 0; i < traits.swizzle.size(); ++i) {
        key |= u64{traits.swizzle[i]} << (Bits::SWIZZLE_SHIFT + 8 * i);
    }
    return key;
}

[[nodiscard]] constexpr SamplerViewTraits UnpackSamplerClassKey(u64 key) noexcept {
    namespace Bits = SamplerClassKeyBits;
    SamplerViewTraits traits{
        .anisotropy = (key & Bits::ANISOTROPY) != 0,
        .integer = (key & Bits::INTEGER) != 0,
        .minmax = (key & Bits::MINMAX) != 0,
        .border_format = (key & Bits::BORDER_FORMAT) != 0,
        .srgb = (key & Bits::SRGB) != 0,
        .identity_swizzle = (key & Bits::IDENTITY_SWIZZLE) != 0,
    };
    if (!traits.identity_swizzle) {
        for (size_t i = 0; i < traits.swizzle.size(); ++i) {
            traits.swizzle[i] = static_cast<u8>(key >> (Bits::SWIZZLE_SHIFT + 8 * i));
        }
    }
    return traits;
}

} // namespace Vulkan
