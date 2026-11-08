// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/frontend/framebuffer_layout.h"

TEST_CASE("21:9 fills 21:9 panels and keeps 21:9 elsewhere", "[framebuffer_layout]") {
    REQUIRE(Layout::UltrawideAspectRatio(1440.0f / 3440.0f) == 1440.0f / 3440.0f);
    REQUIRE(Layout::UltrawideAspectRatio(1080.0f / 2560.0f) == 1080.0f / 2560.0f);
    REQUIRE(Layout::UltrawideAspectRatio(9.0f / 21.0f) == 9.0f / 21.0f);
    REQUIRE(Layout::UltrawideAspectRatio(1440.0f / 2560.0f) == 9.0f / 21.0f);
    REQUIRE(Layout::UltrawideAspectRatio(1080.0f / 3840.0f) == 9.0f / 21.0f);
}

TEST_CASE("A 3440x1440 window at 21:9 shows the game on the whole window", "[framebuffer_layout]") {
    const auto previous = Settings::values.aspect_ratio.GetValue();
    Settings::values.aspect_ratio.SetValue(Settings::AspectRatio::R21_9);
    const auto ultrawide = Layout::DefaultFrameLayout(3440, 1440);
    const auto wide = Layout::DefaultFrameLayout(3840, 1440);
    Settings::values.aspect_ratio.SetValue(previous);

    REQUIRE(ultrawide.screen.left == 0);
    REQUIRE(ultrawide.screen.GetWidth() == 3440);
    REQUIRE(ultrawide.screen.GetHeight() == 1440);
    REQUIRE(wide.screen.GetWidth() == 3360);
    REQUIRE(wide.screen.left == 240);
}
