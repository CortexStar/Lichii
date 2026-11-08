// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/vi/conductor.h"

using Service::VI::ScreenCompositionSpeedScale;

TEST_CASE("ScreenComposition period follows the speed limit", "[vi][conductor]") {
    REQUIRE(ScreenCompositionSpeedScale(true, true, 100.f, false) == 1.f);
    REQUIRE(ScreenCompositionSpeedScale(true, true, 200.f, false) == 0.5f);
    REQUIRE(ScreenCompositionSpeedScale(true, true, 50.f, true) == 2.f);
}

TEST_CASE("ScreenComposition runs free only for unlocked Switch 1 titles", "[vi][conductor]") {
    REQUIRE(ScreenCompositionSpeedScale(true, false, 100.f, false) == 0.01f);
    REQUIRE(ScreenCompositionSpeedScale(true, false, 100.f, true) == 1.f);
}

TEST_CASE("ScreenComposition is not scaled on a single core", "[vi][conductor]") {
    REQUIRE(ScreenCompositionSpeedScale(false, false, 100.f, false) == 1.f);
    REQUIRE(ScreenCompositionSpeedScale(false, true, 200.f, true) == 1.f);
}
