// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "video_core/perf_overlay.h"

namespace {
using namespace VideoCore::PerfOverlay;
using namespace std::chrono_literals;
using Catch::Approx;

std::string_view Row(const Stats& stats, size_t row) {
    return std::string_view{stats.Text().data() + row * Stats::Columns, Stats::Columns};
}
} // namespace

TEST_CASE("Overlay colours are encoded for each output", "[video_core][perf_overlay]") {
    const std::array<float, 3> white{1.0f, 1.0f, 1.0f};
    // SDR: sRGB transfer, independent of the HDR white.
    REQUIRE(EncodeColor(white, 0.0f, Encoding::Srgb)[0] == Approx(1.0f));
    REQUIRE(EncodeColor({0.5f, 0.5f, 0.5f}, 0.0f, Encoding::Srgb)[1] ==
            Approx(0.7354f).margin(1e-3));
    // scRGB: 1.0 is 80 nits.
    REQUIRE(EncodeColor(white, 203.0f, Encoding::ScRgb)[2] == Approx(203.0f / 80.0f));
    // PQ: BT.2408's 203 nit reference white is code 0.58, 10000 nits is 1, black is ~0.
    const auto pq = EncodeColor(white, 203.0f, Encoding::Pq);
    for (const float c : pq) {
        REQUIRE(c == Approx(0.5807f).margin(2e-3));
    }
    REQUIRE(EncodeColor(white, 10000.0f, Encoding::Pq)[0] == Approx(1.0f).margin(1e-3));
    REQUIRE(EncodeColor({0.0f, 0.0f, 0.0f}, 203.0f, Encoding::Pq)[0] < 1e-4f);
    // Pure BT.709 red keeps some green and blue in BT.2020.
    const auto red = EncodeColor({1.0f, 0.0f, 0.0f}, 203.0f, Encoding::Pq);
    REQUIRE(red[0] > red[1]);
    REQUIRE(red[1] > 0.0f);
}

TEST_CASE("Overlay quantiles interpolate like oot_bench", "[video_core][perf_overlay]") {
    std::vector<float> values{4.0f, 1.0f, 3.0f, 2.0f, 5.0f};
    REQUIRE(Quantile(values, 0.5f) == Approx(3.0f));
    REQUIRE(Quantile(values, 0.99f) == Approx(4.96f));
    REQUIRE(Quantile(values, 1.0f) == Approx(5.0f));
    std::vector<float> one{7.0f};
    REQUIRE(Quantile(one, 0.99f) == Approx(7.0f));
    REQUIRE(Quantile(std::span<float>{}, 0.99f) == 0.0f);
}

TEST_CASE("Overlay rates, lows and per-frame thread times", "[video_core][perf_overlay]") {
    Stats stats;
    Stats::Clock::time_point now{};
    REQUIRE(stats.RefreshDue(now));
    stats.Refresh(now, {.threads = {.front = 0, .back = 0}});
    REQUIRE(Row(stats, 1).starts_with("Frame    -- ms"));
    REQUIRE_FALSE(stats.RefreshDue(now + Stats::RefreshInterval - 1ns));
    REQUIRE(stats.RefreshDue(now + Stats::RefreshInterval));

    // 100 presents at 10 ms intervals with one 30 ms hitch (99 frames over 1.02 s), plus one
    // generated frame per present.
    for (int i = 0; i < 100; ++i) {
        now += i == 50 ? 30ms : 10ms;
        stats.OnFrame(now);
    }
    stats.Refresh(now, {
                           .generated_frames = 100,
                           // 2 ms and 3 ms of CPU per real frame; no Vulkan worker.
                           .threads = {.front = 198'000'000, .back = 297'000'000},
                           .gpu_frame_ms = 4.25f,
                       });
    // The first present only starts the clock: 99 frames and 100 generated ones in 1.02 s.
    REQUIRE(Row(stats, 0) == std::string_view{"FPS   97.1 real  195.1 shown      "});
    REQUIRE(Row(stats, 2).starts_with("CPU front  2.00 back  3.00 ms"));
    REQUIRE(Row(stats, 3).starts_with("CPU submit    -- GPU  4.25 ms"));
    // Average 10.2 ms; the 99th percentile sits between 10 ms and the 30 ms hitch.
    REQUIRE(Row(stats, 1).starts_with("Frame 10.20 ms  1% low"));
    REQUIRE(stats.GraphMaxMs() == 21.0f);

    std::array<float, Stats::GraphSamples> graph{};
    stats.Graph(graph);
    REQUIRE(graph[Stats::GraphSamples - 100] == 0.0f);
    REQUIRE(graph.back() == Approx(10.0f / 21.0f));
    // The hitch (frame 50 of the 99 intervals) is marked negative.
    REQUIRE(graph[Stats::GraphSamples - 99 + 49] == Approx(-30.0f / 21.0f));
}

TEST_CASE("Overlay lows cover the last five seconds only", "[video_core][perf_overlay]") {
    Stats stats;
    Stats::Clock::time_point now{};
    stats.OnFrame(now);
    // An old 100 ms hitch, then six seconds of 5 ms frames.
    now += 100ms;
    stats.OnFrame(now);
    for (int i = 0; i < 1200; ++i) {
        now += 5ms;
        stats.OnFrame(now);
    }
    stats.Refresh(now, {});
    REQUIRE(Row(stats, 1).starts_with("Frame  5.00 ms  1% low 200.0 fps"));
}
