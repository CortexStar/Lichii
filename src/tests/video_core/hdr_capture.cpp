// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>
#include <cstdlib>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "video_core/hdr_capture.h"

namespace {

using namespace VideoCore::HdrCapture;

/// Inverse of PqToNits, for building inputs.
f32 NitsToPq(f32 nits) {
    constexpr f32 m1 = 2610.0f / 16384.0f;
    constexpr f32 m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr f32 c1 = 3424.0f / 4096.0f;
    constexpr f32 c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr f32 c3 = 2392.0f / 4096.0f * 32.0f;
    const f32 y = std::pow(nits / 10000.0f, m1);
    return std::pow((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

std::array<u8, 4> Pixel(f32 r, f32 g, f32 b, const Params& params) {
    const f32 rgba[4]{r, g, b, 1.0f};
    std::array<u8, 4> bgra{};
    ConvertPixel(rgba, params, bgra.data());
    return bgra;
}

} // Anonymous namespace

TEST_CASE("HDR capture: PQ decodes to ST 2084 luminance", "[video_core][hdr_capture]") {
    REQUIRE(PqToNits(0.0f) == Catch::Approx(0.0f).margin(1e-4));
    REQUIRE(PqToNits(1.0f) == Catch::Approx(10000.0f).epsilon(1e-4));
    // ITU-R BT.2100 reference values: 100 nits at code 0.5081, 1000 nits at 0.7518.
    REQUIRE(PqToNits(0.5081f) == Catch::Approx(100.0f).epsilon(2e-3));
    REQUIRE(PqToNits(0.7518f) == Catch::Approx(1000.0f).epsilon(2e-3));
}

TEST_CASE("HDR capture: reference white is SDR white, black stays black",
          "[video_core][hdr_capture]") {
    const Params params{.reference_white = 240.0f};
    const f32 white = NitsToPq(240.0f);
    REQUIRE(Pixel(white, white, white, params) == std::array<u8, 4>{255, 255, 255, 255});
    REQUIRE(Pixel(0.0f, 0.0f, 0.0f, params) == std::array<u8, 4>{0, 0, 0, 255});
    // Below the knee the values are the game's: 18 % grey at 0.18 x reference white.
    const f32 grey = NitsToPq(0.18f * 240.0f);
    const auto mid = Pixel(grey, grey, grey, params);
    REQUIRE(std::abs(mid[0] - std::lround(LinearToSrgb(0.18f) * 255.0f)) <= 1);
}

TEST_CASE("HDR capture: highlights clip with their hue", "[video_core][hdr_capture]") {
    REQUIRE(Clip(0.5f) == 0.5f);
    REQUIRE(Clip(4.0f) == 1.0f);
    // BT.2020 red 4x and green 2x reference white: BT.709 (5.47, 1.77, -0.27), scaled down
    // by its red to (1, 0.323, 0), not clipped per channel to (1, 1, 0).
    const auto orange =
        Pixel(NitsToPq(800.0f), NitsToPq(400.0f), NitsToPq(0.0f), {.reference_white = 200.0f});
    REQUIRE(orange[0] == 0);
    REQUIRE(orange[2] == 255);
    REQUIRE(std::abs(orange[1] - std::lround(LinearToSrgb(0.323f) * 255.0f)) <= 2);
}

TEST_CASE("HDR capture: BT.2020 primaries map into BT.709", "[video_core][hdr_capture]") {
    const Params params{.reference_white = 203.0f};
    // BT.2020 green is outside BT.709: red and blue go negative and clip to 0.
    const auto green = Pixel(0.0f, NitsToPq(100.0f), 0.0f, params);
    REQUIRE(green[2] == 0);
    REQUIRE(green[0] == 0);
    // 100 nits of BT.2020 green: 0.49 x reference white x 1.1329 in BT.709 green.
    REQUIRE(std::abs(green[1] - std::lround(LinearToSrgb(100.0f / 203.0f * 1.1329f) * 255.0f)) <=
            2);
}

TEST_CASE("HDR capture: scRGB uses the frame's nits per unit", "[video_core][hdr_capture]") {
    const Params params{.color_space = Tegra::ColorSpace::RGB709_Linear,
                        .reference_white = 160.0f,
                        .nits_per_unit = 80.0f};
    REQUIRE(Pixel(2.0f, 2.0f, 2.0f, params) == std::array<u8, 4>{255, 255, 255, 255});
    const auto half = Pixel(1.0f, 1.0f, 1.0f, params);
    REQUIRE(std::abs(half[0] - std::lround(LinearToSrgb(0.5f) * 255.0f)) <= 1);
}
