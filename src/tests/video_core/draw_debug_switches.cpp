// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string_view>
#include <catch2/catch_test_macros.hpp>

#include "video_core/draw_debug_switches.h"

TEST_CASE("Draw debug hooks stay off without their switches", "[video_core]") {
    CHECK_FALSE(
        VideoCommon::AnyDrawDebugSwitch([](const char*) -> const char* { return nullptr; }));
    // Unrelated switches don't turn them on.
    CHECK_FALSE(VideoCommon::AnyDrawDebugSwitch([](const char* name) -> const char* {
        return std::string_view{name} == "EDEN_NX2_GPU_PROFILE" ? "1" : nullptr;
    }));
}

TEST_CASE("Each draw debug switch turns the hooks on", "[video_core]") {
    for (const char* wanted : VideoCommon::DrawDebugSwitches) {
        CHECK(VideoCommon::AnyDrawDebugSwitch([wanted](const char* name) -> const char* {
            return std::string_view{name} == wanted ? "1" : nullptr;
        }));
    }
}
