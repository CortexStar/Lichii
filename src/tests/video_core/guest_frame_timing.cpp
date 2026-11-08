// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/guest_frame_timing.h"

TEST_CASE("Guest frame latency statistics", "[video_core][pacing]") {
    std::array<double, 1024> samples{};
    for (size_t i = 0; i < 100; ++i) {
        samples[i] = static_cast<double>(100 - i); // 100 .. 1
    }
    const auto stats = VideoCore::GuestFrameTiming::Summarize(samples, 100);
    CHECK(stats.samples == 100);
    CHECK(stats.mean_ms == 50.5);
    CHECK(stats.max_ms == 100.0);
    CHECK(stats.p50_ms == 51.0);
    CHECK(stats.p99_ms == 99.0);
    CHECK(VideoCore::GuestFrameTiming::Summarize(samples, 0).samples == 0);
}
