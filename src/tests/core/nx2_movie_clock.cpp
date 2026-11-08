// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <deque>
#include <optional>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_movie_clock.h"

namespace {
constexpr s64 Frame = 16667;
constexpr s64 Start = 5'000'000;
} // namespace

TEST_CASE("NX2 movie clock ignores frames acquired ahead of it", "[nx2_movie_clock]") {
    // A player with two output images that takes each frame as soon as it is written, a frame
    // ahead of when it shows it.
    Service::Nx2MovieClock clock;
    clock.Acquired(Start, 0, 0);
    for (s64 frame = 1; frame < 600; ++frame) {
        CAPTURE(frame);
        const s64 now = Start + (frame - 1) * Frame + 500;
        clock.Acquired(now, static_cast<u64>(frame * Frame), 0);
        REQUIRE(clock.Due(now, 0) == static_cast<u64>(now - Start));
    }
}

TEST_CASE("NX2 movie clock runs on while a slow client falls behind", "[nx2_movie_clock]") {
    // Ocarina of Time: one 60 fps movie frame per frame drawn, at 48 fps.
    Service::Nx2MovieClock clock;
    for (s64 frame = 0; frame < 120; ++frame) {
        const s64 now = Start + frame * 20833;
        clock.Acquired(now, static_cast<u64>(frame * Frame), 0);
        REQUIRE(clock.Due(now, 0) == static_cast<u64>(now - Start));
    }
}

TEST_CASE("NX2 movie clock restarts after a pause, a loop or a long lag", "[nx2_movie_clock]") {
    using Clock = Service::Nx2MovieClock;
    Clock clock;
    REQUIRE_FALSE(clock.Due(Start, 0));
    clock.Acquired(Start, 0, 0);
    clock.Acquired(Start + Frame, Frame, 0);

    // No acquire for over a second: no due pts until the next acquire restarts the clock.
    s64 now = Start + Frame + Clock::PauseGap + 1;
    REQUIRE_FALSE(clock.Due(now, 0));
    clock.Acquired(now, 2 * Frame, 0);
    REQUIRE(clock.Due(now + 1000, 0) == 2 * Frame + 1000);

    // Emulation paused and resumed since the last acquire.
    const s64 resumed = now + 2000;
    REQUIRE_FALSE(clock.Due(now + 3000, resumed));
    now += 4000;
    clock.Acquired(now, 3 * Frame, resumed);
    REQUIRE(clock.Due(now, resumed) == 3 * Frame);

    // The movie loops back to its start.
    now += Frame;
    clock.Acquired(now, 0, resumed);
    REQUIRE(clock.Due(now, resumed) == 0);

    // The client takes a frame every half second: once it lags by more than MaxCatchUp, the
    // clock restarts from its frame.
    const s64 loop = now;
    for (s64 step = 1; step <= 7; ++step) {
        now = loop + step * Clock::PauseGap / 2;
        clock.Acquired(now, static_cast<u64>(step * Frame), resumed);
        const s64 lag = (now - loop) - step * Frame;
        CAPTURE(step, lag);
        REQUIRE(clock.Due(now, resumed) ==
                static_cast<u64>(lag > Clock::MaxCatchUp ? step * Frame : now - loop));
    }

    clock.Reset();
    REQUIRE_FALSE(clock.Due(now, resumed));
}

// Holding frames for clients faster than the movie (HoldUs).
namespace {
using Service::Nx2MovieClock;
constexpr u64 HoldFrame = 16'667;
constexpr u64 Tolerance = HoldFrame / 2;
} // namespace

TEST_CASE("NX2 movie clock paces writes before the first acquire", "[nx2_movie_clock]") {
    Nx2MovieClock clock;
    // Nothing written yet: nothing to pace by.
    REQUIRE_FALSE(clock.HoldUs(Start, 0, 0, Tolerance));
    clock.Wrote(Start, 0, 0);
    // The second frame waits for its time, not for the client.
    REQUIRE(clock.HoldUs(Start + 1'000, HoldFrame, 0, Tolerance));
    REQUIRE_FALSE(clock.HoldUs(Start + HoldFrame - Tolerance, HoldFrame, 0, Tolerance));
    // The first acquire starts the movie's clock, which takes over.
    clock.Acquired(Start + 20'000, 0, 0);
    REQUIRE(clock.HoldUs(Start + 20'000, HoldFrame, 0, Tolerance));
    REQUIRE(*clock.HoldUs(Start + 20'000, HoldFrame, 0, Tolerance) == HoldFrame - Tolerance);
}

TEST_CASE("NX2 movie clock holds frames a fast client asks for early", "[nx2_movie_clock]") {
    Nx2MovieClock clock;
    REQUIRE_FALSE(clock.HoldUs(Start, 10 * HoldFrame, 0, Tolerance));
    clock.Acquired(Start, 0, 0);
    // A client at 240 fps asks 4 ms later: frame 1 is due in 12.7 ms, more than half a frame.
    REQUIRE(clock.HoldUs(Start + 4'000, HoldFrame, 0, Tolerance));
    // Within half a frame it goes out.
    REQUIRE_FALSE(clock.HoldUs(Start + 9'000, HoldFrame, 0, Tolerance));
    // Handing it out early leaves the clock where it was.
    clock.Acquired(Start + 9'000, HoldFrame, 0);
    REQUIRE(clock.Due(Start + 20'000, 0) == 20'000);
    REQUIRE(clock.HoldUs(Start + 20'000, 2 * HoldFrame, 0, Tolerance));
    // Over a pause nothing is held: the next acquire restarts the clock.
    REQUIRE_FALSE(clock.HoldUs(Start + 21'000, 5 * HoldFrame, Start + 20'500, Tolerance));
}

namespace {
using Service::Nx2MovieRateMeter;

enum class Hold {
    BeforeWrite, ///< The session holds an early frame before writing it (the fix)
    AtHandout,   ///< It writes every frame and answers an early 213 with "no frame"
    None,        ///< No hold (before 69d58bbc38): the client takes every written frame
};

struct Playback {
    u64 acquired;                                ///< Frames the client acquired
    s64 longest_gap;                             ///< Between two acquires, us
    std::optional<Nx2MovieRateMeter::Report> rate; ///< The first full rate window
};

// The decoder session and a client that draws every `client_period` us, as Ocarina of Time does:
// it asks for a frame (213) only once a new output fence has passed (the decoder's syncpoint
// counts written frames), takes the oldest written frame and gives back the one it showed.
// Decoded frames are always available (60 fps stream).
Playback Simulate(s64 client_period, u32 images, s64 seconds, Hold hold) {
    Nx2MovieClock clock;
    Nx2MovieRateMeter meter;
    std::deque<u64> written;
    u32 free_images = images;
    u64 next_pts = 0;
    u64 fences = 0;
    u64 asked = 0;
    bool holding_one = false;
    s64 last_acquire = Start;
    Playback result{};
    for (s64 now = Start; now < Start + seconds * 1'000'000; now += 500) {
        // Decode thread: it wakes when a hold ends, modelled at 0.5 ms granularity.
        while (free_images > 0 &&
               !(hold == Hold::BeforeWrite && clock.HoldUs(now, next_pts, 0, Tolerance))) {
            written.push_back(next_pts);
            clock.Wrote(now, next_pts, 0);
            next_pts += HoldFrame;
            --free_images;
            ++fences;
        }
        if ((now - Start) % client_period >= 500 || asked == fences) {
            continue;
        }
        ++asked;
        if (written.empty() ||
            (hold == Hold::AtHandout && clock.HoldUs(now, written.front(), 0, Tolerance))) {
            continue; // "no frame": the client waits for the next fence
        }
        clock.Acquired(now, written.front(), 0);
        if (const auto report = meter.Shown(now, written.front()); report && !result.rate) {
            result.rate = report;
        }
        written.pop_front();
        if (holding_one) {
            ++free_images;
        }
        holding_one = true;
        ++result.acquired;
        result.longest_gap = std::max(result.longest_gap, now - last_acquire);
        last_acquire = now;
    }
    result.longest_gap =
        std::max(result.longest_gap, Start + seconds * 1'000'000 - last_acquire);
    return result;
}

// Client periods: 60, 112, 180 and 224 Hz (the frame rate limit and FPS cap settings in use).
constexpr std::array ClientPeriods{s64{16'500}, s64{8'929}, s64{5'556}, s64{4'464}};
} // namespace

TEST_CASE("NX2 movie plays at its own rate for clients at 60, 112, 180 and 224 Hz",
          "[nx2_movie_clock]") {
    for (const u32 images : {2U, 3U, 4U}) {
        for (const s64 period : ClientPeriods) {
            const auto played = Simulate(period, images, 12, Hold::BeforeWrite);
            INFO("client period " << period << " us, " << images << " images");
            // 12 s of a 60 fps movie, never stalled for more than two movie frames.
            REQUIRE(played.acquired >= 12 * 59);
            REQUIRE(played.acquired <= 12 * 61);
            REQUIRE(played.longest_gap <= 2 * static_cast<s64>(HoldFrame) + period);
            // The rate log's first window: 60 fps within 5%, movie time = wall time.
            REQUIRE(played.rate.has_value());
            const double fps = static_cast<double>(played.rate->frames) * 1e6 /
                               static_cast<double>(played.rate->wall_us);
            const double speed = static_cast<double>(played.rate->movie_us) /
                                 static_cast<double>(played.rate->wall_us);
            REQUIRE(fps > 57.0);
            REQUIRE(fps < 63.0);
            REQUIRE(speed > 0.95);
            REQUIRE(speed < 1.05);
        }
    }
}

TEST_CASE("NX2 movie without a hold plays as fast as the client draws",
          "[nx2_movie_clock]") {
    // Before 69d58bbc38 Ocarina played its 60 fps title movie at the rate it draws: on the PC
    // (FPS limit 224, vsync 240) 600 frames in 2.9 s.
    for (const s64 period : {s64{8'929}, s64{4'464}}) {
        const auto played = Simulate(period, 2, 12, Hold::None);
        INFO("client period " << period << " us");
        REQUIRE(played.rate.has_value());
        const double speed = static_cast<double>(played.rate->movie_us) /
                             static_cast<double>(played.rate->wall_us);
        REQUIRE(speed > 1.5);
    }
}

TEST_CASE("NX2 movie answering an early frame request with no frame stalls the client",
          "[nx2_movie_clock]") {
    // The first design held frames at 213 after writing them: the client spent the fence on an
    // empty answer and, with every image written, no fence came again. Ocarina's title movie
    // showed nothing after frame 1 at 171-180 fps.
    for (const u32 images : {2U, 4U}) {
        const auto played = Simulate(5'500, images, 10, Hold::AtHandout);
        INFO(images << " images");
        REQUIRE(played.acquired <= images);
        REQUIRE(played.longest_gap > 9'000'000);
    }
}

TEST_CASE("NX2 movie rate meter reports full windows and restarts", "[nx2_movie_clock]") {
    Nx2MovieRateMeter meter;
    std::optional<Nx2MovieRateMeter::Report> report;
    u64 pts = 0;
    s64 now = Start;
    for (; !report; now += 16'667, pts += 16'667) {
        report = meter.Shown(now, pts);
    }
    REQUIRE(report->frames == 601);
    REQUIRE(report->wall_us >= Nx2MovieRateMeter::Window);
    REQUIRE(report->movie_us == static_cast<u64>(report->wall_us));
    // A restart at pts 0 closes the window early (over a second here).
    for (int i = 0; i < 120; ++i, now += 16'667, pts += 16'667) {
        REQUIRE_FALSE(meter.Shown(now, pts).has_value());
    }
    report = meter.Shown(now, 0);
    REQUIRE(report.has_value());
    REQUIRE(report->frames == 120);
    // Stopping reports a window of a second or more, then nothing until frames come again.
    for (int i = 0; i < 70; ++i) {
        now += 16'667;
        REQUIRE_FALSE(meter.Shown(now, static_cast<u64>(i + 1) * 16'667).has_value());
    }
    REQUIRE(meter.Finish(now).has_value());
    REQUIRE_FALSE(meter.Finish(now + 2'000'000).has_value());
}
