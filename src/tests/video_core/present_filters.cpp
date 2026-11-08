// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/present.h"

TEST_CASE("Present: no scaling filter on an upscaler's image shown 1:1", "[video_core][present]") {
    // FSR 4 Native AA at 3440x1440 on a 3440x1440 screen: the plain copy.
    CHECK(SkipsScalingFilter(true, 3440, 1440, 3440, 1440));
    // Scaled to the screen, or not an upscaler's image: the user's filter runs.
    CHECK_FALSE(SkipsScalingFilter(true, 2560, 1440, 3440, 1440));
    CHECK_FALSE(SkipsScalingFilter(true, 2560, 1440, 3840, 2160));
    CHECK_FALSE(SkipsScalingFilter(false, 3440, 1440, 3440, 1440));
}
