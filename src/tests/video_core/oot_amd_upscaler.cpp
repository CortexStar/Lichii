// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/oot_dlss.h"

TEST_CASE("OoT AMD upscaler: FSR 4 first, FSR 3.1 on the tonemapped image only",
          "[video_core][oot]") {
    using VideoCommon::OotDlss::AmdUpscaler;
    using VideoCommon::OotDlss::PickAmdUpscaler;
    CHECK(PickAmdUpscaler(true, true, true) == AmdUpscaler::Fsr4);
    CHECK(PickAmdUpscaler(true, false, false) == AmdUpscaler::Fsr4);
    CHECK(PickAmdUpscaler(false, false, true) == AmdUpscaler::Fsr31);
    CHECK(PickAmdUpscaler(false, true, true) == AmdUpscaler::None);
    CHECK(PickAmdUpscaler(false, false, false) == AmdUpscaler::None);
}

TEST_CASE("OoT upscaler LOD bias: authored textures only", "[video_core][oot]") {
    using VideoCommon::OotDlss::UpscaleLodBiasApplies;
    using VideoCore::Surface::PixelFormat;
    // Authored textures: ASTC (re-compressed or not on the host) and BCn.
    CHECK(UpscaleLodBiasApplies(PixelFormat::ASTC_2D_4X4_UNORM));
    CHECK(UpscaleLodBiasApplies(PixelFormat::ASTC_2D_8X8_SRGB));
    CHECK(UpscaleLodBiasApplies(PixelFormat::BC7_UNORM));
    CHECK(UpscaleLodBiasApplies(PixelFormat::BC1_RGBA_UNORM));
    // Images the game renders and reads back with explicit LODs (bloom, fog, scene colour).
    CHECK_FALSE(UpscaleLodBiasApplies(PixelFormat::B10G11R11_FLOAT));
    CHECK_FALSE(UpscaleLodBiasApplies(PixelFormat::R16G16B16A16_FLOAT));
    CHECK_FALSE(UpscaleLodBiasApplies(PixelFormat::A8B8G8R8_UNORM));
    CHECK_FALSE(UpscaleLodBiasApplies(PixelFormat::D32_FLOAT));
}
