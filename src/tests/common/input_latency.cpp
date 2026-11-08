// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "common/input_latency.h"

using Catch::Matchers::WithinAbs;
using Common::InputLatency::MaxSamples;
using Common::InputLatency::Summarize;
using Common::InputLatency::Tracker;

TEST_CASE("A frame samples the earliest change since the previous frame start",
          "[common][input_latency]") {
    Tracker tracker;
    CHECK(tracker.FrameStart() == 0);
    tracker.Changed(100);
    tracker.Changed(150);
    CHECK(tracker.FrameStart() == 100);
    // The next frame saw nothing new.
    CHECK(tracker.FrameStart() == 0);
    tracker.Changed(300);
    CHECK(tracker.FrameStart() == 300);
}

TEST_CASE("Each change is measured once, at the first present that shows it",
          "[common][input_latency]") {
    Tracker tracker;
    CHECK_FALSE(tracker.Present(0, 1000).has_value());
    CHECK(tracker.Present(100, 1100) == 1000u);
    // The game presented the same frame's change again (two presents in one vsync).
    CHECK_FALSE(tracker.Present(100, 1200).has_value());
    // A clock that went backwards gives no sample.
    CHECK_FALSE(tracker.Present(5000, 4000).has_value());
    CHECK(tracker.Present(2000, 2500) == 500u);
}

TEST_CASE("Input latency statistics", "[common][input_latency]") {
    std::array<double, MaxSamples> samples{};
    CHECK(Summarize(samples, 0).samples == 0);

    samples[0] = 8.0;
    samples[1] = 2.0;
    samples[2] = 4.0;
    samples[3] = 6.0;
    const auto stats = Summarize(samples, 4);
    CHECK(stats.samples == 4);
    CHECK_THAT(stats.mean_ms, WithinAbs(5.0, 1e-9));
    CHECK_THAT(stats.p50_ms, WithinAbs(5.0, 1e-9));
    CHECK_THAT(stats.max_ms, WithinAbs(8.0, 1e-9));

    // A wrapped ring: every slot holds a sample.
    samples.fill(3.0);
    CHECK(Summarize(samples, MaxSamples * 3).samples == MaxSamples);
}
