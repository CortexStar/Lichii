// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <random>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/sampler_class_key.h"

using Vulkan::PackSamplerClassKey;
using Vulkan::SamplerViewTraits;
using Vulkan::UnpackSamplerClassKey;

TEST_CASE("Sampler class keys keep every trait the variant reads", "[video_core][sampler]") {
    std::mt19937 random{0x5a3};
    for (unsigned i = 0; i < 20000; ++i) {
        SamplerViewTraits traits{
            .anisotropy = (random() & 1) != 0,
            .integer = (random() & 1) != 0,
            .minmax = (random() & 1) != 0,
            .border_format = (random() & 1) != 0,
            .srgb = (random() & 1) != 0,
            .authored = (random() & 1) != 0,
            .identity_swizzle = (random() & 1) != 0,
        };
        if (!traits.identity_swizzle) {
            // VkComponentSwizzle: IDENTITY .. A (0..6)
            for (auto& component : traits.swizzle) {
                component = static_cast<u8>(random() % 7);
            }
        }
        REQUIRE(UnpackSamplerClassKey(PackSamplerClassKey(traits)) == traits);
    }
}

TEST_CASE("Identity swizzles share one class whatever the components", "[video_core][sampler]") {
    SamplerViewTraits a{.srgb = true, .identity_swizzle = true, .swizzle = {1, 2, 3, 4}};
    SamplerViewTraits b{.srgb = true, .identity_swizzle = true};
    STATIC_REQUIRE(PackSamplerClassKey(SamplerViewTraits{.identity_swizzle = true}) != 0);
    CHECK(PackSamplerClassKey(a) == PackSamplerClassKey(b));
    SamplerViewTraits c = b;
    c.identity_swizzle = false;
    c.swizzle = {3, 4, 5, 6};
    CHECK(PackSamplerClassKey(c) != PackSamplerClassKey(b));
    c.integer = true;
    CHECK(UnpackSamplerClassKey(PackSamplerClassKey(c)).integer);
}
