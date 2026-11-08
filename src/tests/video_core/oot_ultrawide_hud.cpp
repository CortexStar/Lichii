// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>

#include <catch2/catch_test_macros.hpp>

#include "video_core/oot_ultrawide_hud.h"

using namespace VideoCommon::OotUltrawideHud;

namespace {
bool Near(Span a, Span b) {
    return std::abs(a.x - b.x) < 1e-3f && std::abs(a.width - b.width) < 1e-3f;
}
} // Anonymous namespace

TEST_CASE("Ocarina's HUD band is the centred 16:9 part of the frame", "[oot_ultrawide_hud]") {
    REQUIRE(HudBand(3440, 1440) == Band{440, 2560});
    REQUIRE(HudBand(2560, 1080) == Band{320, 1920});
    REQUIRE(HudBand(5120, 2160) == Band{640, 3840});
    REQUIRE(HudBand(2560, 1440) == Band{0, 2560});
    REQUIRE(HudBand(1920, 1200) == Band{0, 1920});
}

TEST_CASE("HUD viewports and scissors move into the band", "[oot_ultrawide_hud]") {
    const Band band = HudBand(3440, 1440);
    REQUIRE(Near(IntoBand(Span{0.0f, 3440.0f}, 3440, band), Span{440.0f, 2560.0f}));
    REQUIRE(Near(IntoBand(Span{1720.0f, 1720.0f}, 3440, band), Span{1720.0f, 1280.0f}));
    REQUIRE(IntoBand(ScissorSpan{0, 3440}, 3440, band) == ScissorSpan{440, 2560});
    // A scissor around an icon at the right edge keeps every pixel the icon reaches.
    REQUIRE(IntoBand(ScissorSpan{3000, 101}, 3440, band) == ScissorSpan{2672, 76});
    // 16:9 frames are left alone.
    const Band whole = HudBand(2560, 1440);
    REQUIRE(IntoBand(Span{10.0f, 20.0f}, 2560, whole) == Span{10.0f, 20.0f});
    REQUIRE(IntoBand(ScissorSpan{10, 20}, 2560, whole) == ScissorSpan{10, 20});
}

TEST_CASE("Full-width HUD panes keep the whole frame", "[oot_ultrawide_hud]") {
    REQUIRE(IsFullWidthPane(0.0f, 1920.0f));
    REQUIRE(IsFullWidthPane(-100.0f, 2100.0f));
    REQUIRE_FALSE(IsFullWidthPane(0.0f, 1900.0f));
    REQUIRE_FALSE(IsFullWidthPane(40.0f, 1920.0f));
}

TEST_CASE("HUD markers follow the band", "[oot_ultrawide_hud]") {
    const auto ultrawide = MarkerMapping(3440, HudBand(3440, 1440));
    REQUIRE(std::abs(ultrawide.scale - 2580.0f) < 1e-3f);
    REQUIRE(std::abs(ultrawide.offset - 330.0f) < 1e-3f);
    REQUIRE(MarkerMapping(2560, HudBand(2560, 1440)) == MarkerMap{1920.0f, 0.0f});
    // A target at the band's left edge lands on the canvas's left edge.
    const auto map = MarkerMapping(3440, HudBand(3440, 1440));
    REQUIRE(std::abs(440.0f / 3440.0f * map.scale - map.offset) < 1e-3f);
}
