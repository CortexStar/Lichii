// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <deque>
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
// The decoder session and a client that draws every `client_period` us, as Ocarina of Time does:
// it asks for a frame (213) only once a new output fence has passed (the decoder's syncpoint
// counts written frames), takes the oldest written frame and gives back the one it showed.
// `hold_before_write`: the session holds an early frame before writing it; otherwise it writes
// every frame at once and answers an early 213 with "no frame". Returns the frames the client
// acquired in `seconds` and the longest gap between two acquires (us).
std::pair<u64, s64> Simulate(s64 client_period, u32 images, s64 seconds, bool hold_before_write) {
    Nx2MovieClock clock;
    std::deque<u64> written;
    u32 free_images = images;
    u64 next_pts = 0;
    u64 fences = 0;
    u64 asked = 0;
    u64 acquired = 0;
    bool holding_one = false;
    s64 last_acquire = Start;
    s64 longest_gap = 0;
    for (s64 now = Start; now < Start + seconds * 1'000'000; now += 500) {
        // Decode thread: it wakes when a hold ends, modelled at 0.5 ms granularity.
        while (free_images > 0 &&
               !(hold_before_write && clock.HoldUs(now, next_pts, 0, Tolerance))) {
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
            (!hold_before_write && clock.HoldUs(now, written.front(), 0, Tolerance))) {
            continue; // "no frame": the client waits for the next fence
        }
        clock.Acquired(now, written.front(), 0);
        written.pop_front();
        if (holding_one) {
            ++free_images;
        }
        holding_one = true;
        ++acquired;
        longest_gap = std::max(longest_gap, now - last_acquire);
        last_acquire = now;
    }
    longest_gap = std::max(longest_gap, Start + seconds * 1'000'000 - last_acquire);
    return {acquired, longest_gap};
}
} // namespace

TEST_CASE("NX2 movie plays at its own rate for clients at 60, 180 and 240 fps",
          "[nx2_movie_clock]") {
    for (const u32 images : {2U, 3U, 4U}) {
        for (const s64 period : {s64{16'500}, s64{5'500}, s64{4'000}}) {
            const auto [acquired, longest_gap] = Simulate(period, images, 10, true);
            INFO("client period " << period << " us, " << images << " images");
            // 10 s of a 60 fps movie, never stalled for more than two movie frames.
            REQUIRE(acquired >= 590);
            REQUIRE(acquired <= 610);
            REQUIRE(longest_gap <= 2 * static_cast<s64>(HoldFrame) + period);
        }
    }
}

TEST_CASE("NX2 movie answering an early frame request with no frame stalls the client",
          "[nx2_movie_clock]") {
    // Holding frames at 213 after writing them: the client spent the fence on an empty answer
    // and, with every image written, no fence came again. Ocarina's title movie showed nothing
    // after frame 1 at 171-180 fps.
    for (const u32 images : {2U, 4U}) {
        const auto [acquired, longest_gap] = Simulate(5'500, images, 10, false);
        INFO(images << " images");
        REQUIRE(acquired <= images);
        REQUIRE(longest_gap > 9'000'000);
    }
}
