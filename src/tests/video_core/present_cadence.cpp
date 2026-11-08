// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present_cadence.h"

using namespace std::chrono_literals;
using Vulkan::PresentCadence;
using Vulkan::PresentCadenceRate;

TEST_CASE("Present cadence rate is the VRR cap or the guest's slower pace",
          "[video_core][present_cadence]") {
    REQUIRE(PresentCadenceRate(0, 60) == 0);      // no VRR: no cadence
    REQUIRE(PresentCadenceRate(224, 0) == 224);   // unpaced guest: the cap
    REQUIRE(PresentCadenceRate(224, 210) == 210); // a 30 fps movie's multiple
    REQUIRE(PresentCadenceRate(224, 240) == 224);
    REQUIRE(PresentCadenceRate(224, 60) == 60); // speed limit on
}

TEST_CASE("Present cadence ticks at a constant interval", "[video_core][present_cadence]") {
    PresentCadence cadence;
    const auto start = PresentCadence::Clock::time_point{} + 1s;
    auto tick = cadence.Next(start, 224);
    REQUIRE(tick == start);
    const auto interval = cadence.Interval();
    REQUIRE(interval == std::chrono::nanoseconds{4'464'285});
    for (int i = 0; i < 1000; ++i) {
        // Waking up a little late does not move the ticks.
        const auto next = cadence.Next(tick + 100us, 224);
        REQUIRE(next - tick == interval);
        tick = next;
    }
}

TEST_CASE("Present cadence keeps ticking through a long gap without bursting",
          "[video_core][present_cadence]") {
    PresentCadence cadence;
    const auto start = PresentCadence::Clock::time_point{} + 1s;
    const auto tick = cadence.Next(start, 224);
    // The thread was away for 50 ms: the next tick is now, not ten ticks in the past.
    const auto late = tick + 50ms;
    REQUIRE(cadence.Next(late, 224) == late);
    REQUIRE(cadence.Next(late, 224) - late == cadence.Interval());
}

TEST_CASE("Present cadence slews to a new rate", "[video_core][present_cadence]") {
    PresentCadence cadence;
    auto now = PresentCadence::Clock::time_point{} + 1s;
    now = cadence.Next(now, 224);
    auto previous = cadence.Interval();
    int ticks = 0;
    while (cadence.Interval() != std::chrono::nanoseconds{5'555'555}) {
        now = cadence.Next(now, 180);
        const double step =
            static_cast<double>(cadence.Interval().count()) / static_cast<double>(previous.count());
        // Interval() truncates to whole nanoseconds.
        REQUIRE(step <= 1.0 + PresentCadence::MaxStep + 1e-6);
        previous = cadence.Interval();
        REQUIRE(++ticks < 100);
    }
    // ln(5.556 / 4.464) / ln(1.005): 44 ticks, about 0.2 s.
    REQUIRE(ticks >= 40);
    cadence.Reset();
    REQUIRE(cadence.Next(now, 60) == now);
    REQUIRE(cadence.Interval() == std::chrono::nanoseconds{16'666'666});
}
