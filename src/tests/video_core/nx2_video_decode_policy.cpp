// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/nx2_video_decode_policy.h"

TEST_CASE("NX2 CPU AV1 decode threads scale with the stream", "[video_core][nx2_video]") {
    using Tegra::Host1x::Nx2SoftwareDecodeThreads;
    // Ocarina of Time's movies (1920x1080 at 60) on 16 and 8 logical CPUs: two threads.
    REQUIRE(Nx2SoftwareDecodeThreads(1920, 1080, 60, 16) == 2);
    REQUIRE(Nx2SoftwareDecodeThreads(1920, 1080, 60, 8) == 2);
    // Rate not given: taken as 60.
    REQUIRE(Nx2SoftwareDecodeThreads(1920, 1080, 0, 16) == 2);
    // Smaller or slower streams never go below two.
    REQUIRE(Nx2SoftwareDecodeThreads(1280, 720, 30, 16) == 2);
    // 1440p60 is 1.8 units, 4K60 four.
    REQUIRE(Nx2SoftwareDecodeThreads(2560, 1440, 60, 16) == 4);
    REQUIRE(Nx2SoftwareDecodeThreads(3840, 2160, 60, 16) == 8);
    // Never more than half the logical CPUs (but at least two).
    REQUIRE(Nx2SoftwareDecodeThreads(3840, 2160, 60, 8) == 4);
    REQUIRE(Nx2SoftwareDecodeThreads(3840, 2160, 60, 2) == 2);
}
