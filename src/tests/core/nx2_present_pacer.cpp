// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present_pacer.h"

namespace {
using namespace std::chrono_literals;
using Pacer = Service::Nx2PresentPacer;
constexpr auto Start = Pacer::Clock::time_point{1s};
constexpr auto OneVblank = 16'666'666ns;
constexpr auto TwoVblanks = 33'333'333ns;
constexpr auto ThreeVblanks = 50'000'000ns;
}

TEST_CASE("NX2 FPS limit only lengthens a vblank it is below", "[core][nx2_present_pacer]") {
    using Service::PacedInterval;
    // 0 leaves the game's own vblank and swap interval untouched.
    REQUIRE(PacedInterval(OneVblank, 0) == OneVblank);
    REQUIRE(PacedInterval(TwoVblanks, 0) == TwoVblanks);
    // 60 Hz is exactly the one-vblank interval; 120 Hz is faster than the game asked for.
    REQUIRE(PacedInterval(OneVblank, 60) == OneVblank);
    REQUIRE(PacedInterval(OneVblank, 120) == OneVblank);
    REQUIRE(PacedInterval(TwoVblanks, 60) == TwoVblanks);
    // Lower limits hold a frame for longer: 50, 30, 20 and 15 Hz from one 60 Hz vblank.
    REQUIRE(PacedInterval(OneVblank, 50) == 20'000'000ns);
    REQUIRE(PacedInterval(OneVblank, 30) == 33'333'333ns);
    REQUIRE(PacedInterval(OneVblank, 20) == 50'000'000ns);
    REQUIRE(PacedInterval(OneVblank, 15) == 66'666'666ns);
    // A game already pacing at two vblanks keeps that when the limit is not slower still.
    REQUIRE(PacedInterval(TwoVblanks, 40) == TwoVblanks);
    REQUIRE(PacedInterval(TwoVblanks, 20) == 50'000'000ns);
}

TEST_CASE("NX2 FPS limit is lowered to the host's VRR cap", "[core][nx2_present_pacer]") {
    using Service::EffectiveFpsLimit;
    REQUIRE(EffectiveFpsLimit(0, 0) == 0);
    REQUIRE(EffectiveFpsLimit(60, 0) == 60);
    REQUIRE(EffectiveFpsLimit(0, 224) == 224);
    REQUIRE(EffectiveFpsLimit(240, 224) == 224);
    REQUIRE(EffectiveFpsLimit(120, 224) == 120);
}

TEST_CASE("NX2 present word paces only at the console's vsync rate", "[core][nx2_present_pacer]") {
    using Service::PresentWordInterval;
    // At 60 Hz the word's swap interval paces, scaled by the speed limit.
    REQUIRE(PresentWordInterval(1, 60, 60, 100) == OneVblank);
    REQUIRE(PresentWordInterval(2, 60, 60, 100) == TwoVblanks);
    REQUIRE(PresentWordInterval(1, 60, 60, 200) == 8'333'333ns);
    REQUIRE(PresentWordInterval(1, 60, 60, 50) == TwoVblanks);
    // Speed limit off: only an FPS limit can pace.
    REQUIRE(PresentWordInterval(1, 60, 60, 0) == 0ns);
    // A faster or slower vsync event paces the game by itself, whatever the word says.
    REQUIRE(PresentWordInterval(2, 60, 120, 100) == 0ns);
    REQUIRE(PresentWordInterval(1, 60, 240, 100) == 0ns);
    REQUIRE(PresentWordInterval(1, 60, 30, 100) == 0ns);
    // An FPS limit still holds a timer-paced game: 240 Hz event, limit 120 or 60.
    REQUIRE(Service::PacedInterval(PresentWordInterval(2, 60, 240, 100), 120) == 8'333'333ns);
    REQUIRE(Service::PacedInterval(PresentWordInterval(2, 60, 120, 100), 60) == OneVblank);
}

TEST_CASE("NX2 interval decreases apply to the current present", "[core][nx2_present_pacer]") {
    Pacer pacer;
    REQUIRE(pacer.Advance(Start, ThreeVblanks).reset);
    const auto step = pacer.Advance(Start + 2'679us, TwoVblanks);
    REQUIRE_FALSE(step.reset);
    REQUIRE(step.deadline == Start + TwoVblanks);
    REQUIRE(step.deadline - (Start + 2'679us) == 30'654'333ns);
    REQUIRE(pacer.Advance(step.deadline + 1ms, OneVblank).deadline == step.deadline + OneVblank);
}

TEST_CASE("NX2 interval increases apply to the current present", "[core][nx2_present_pacer]") {
    Pacer pacer;
    pacer.Advance(Start, OneVblank);
    const auto step = pacer.Advance(Start + 1ms, ThreeVblanks);
    REQUIRE_FALSE(step.reset);
    REQUIRE(step.deadline == Start + ThreeVblanks);
    REQUIRE(pacer.Advance(step.deadline + 1ms, TwoVblanks).deadline == step.deadline + TwoVblanks);
}

TEST_CASE("NX2 present period changes retain their existing phase", "[core][nx2_present_pacer]") {
    Pacer pacer;
    auto deadline = pacer.Advance(Start, OneVblank).deadline;
    for (const auto period : {TwoVblanks, OneVblank, 8'333'333ns, ThreeVblanks, OneVblank}) {
        const auto next = pacer.Advance(deadline + 1ms, period);
        REQUIRE_FALSE(next.reset);
        REQUIRE(next.deadline == deadline + period);
        deadline = next.deadline;
    }
}

TEST_CASE("NX2 late presents keep the existing bounded catch-up policy", "[core][nx2_present_pacer]") {
    Pacer pacer;
    pacer.Advance(Start, 10ms);
    const auto late = pacer.Advance(Start + 20ms, 10ms);
    REQUIRE_FALSE(late.reset);
    REQUIRE(late.deadline == Start + 10ms);
    REQUIRE(pacer.Advance(Start + 20ms, 10ms).deadline == Start + 20ms);
    const auto reset = pacer.Advance(Start + 41ms, 10ms);
    REQUIRE(reset.reset);
    REQUIRE(reset.deadline == Start + 41ms);
    REQUIRE(pacer.Advance(Start + 41ms, 10ms).deadline == Start + 51ms);
}

TEST_CASE("NX2 late period changes reanchor using the current period", "[core][nx2_present_pacer]") {
    Pacer pacer;
    pacer.Advance(Start, ThreeVblanks);
    const auto late = pacer.Advance(Start + 40ms, OneVblank);
    REQUIRE(late.reset);
    REQUIRE(late.deadline == Start + 40ms);
    const auto next = pacer.Advance(Start + 40ms, OneVblank);
    REQUIRE_FALSE(next.reset);
    REQUIRE(next.deadline == late.deadline + OneVblank);
}

TEST_CASE("NX2 fixed-period pacing matches the previous algorithm", "[core][nx2_present_pacer]") {
    for (const auto period : {OneVblank, TwoVblanks, std::chrono::nanoseconds{ThreeVblanks}}) {
        Pacer pacer;
        Pacer::Clock::time_point old_next{};
        auto now = Start;
        for (const auto advance : {0ms, 1ms, 16ms, 3ms, 50ms, 0ms, 100ms, 2ms, 40ms}) {
            now += advance;
            const bool old_reset = now > old_next + period;
            if (old_reset) {
                old_next = now;
            }
            const auto step = pacer.Advance(now, period);
            REQUIRE(step.reset == old_reset);
            REQUIRE(step.deadline == old_next);
            now = (std::max)(now, step.deadline);
            old_next += period;
        }
    }
}

TEST_CASE("NX2 movie cadence paces at a multiple of the movie rate", "[core][nx2_present_pacer]") {
    using Service::MovieCadenceRate;
    // No movie, or no limit: unchanged.
    REQUIRE(MovieCadenceRate(112, 0) == 112);
    REQUIRE(MovieCadenceRate(0, 60) == 0);
    // A 60 fps movie: 112 -> 60, 224 -> 180, 240 -> 240, 120 -> 120.
    REQUIRE(MovieCadenceRate(112, 60) == 60);
    REQUIRE(MovieCadenceRate(224, 60) == 180);
    REQUIRE(MovieCadenceRate(240, 60) == 240);
    REQUIRE(MovieCadenceRate(120, 60) == 120);
    // Below the movie's rate the late frames are dropped instead: unchanged.
    REQUIRE(MovieCadenceRate(50, 60) == 50);
    // 30 fps movies.
    REQUIRE(MovieCadenceRate(112, 30) == 90);
}

TEST_CASE("NX2 display start rate follows the host cap when the frame rate is unlocked",
          "[core][nx2_present_pacer]") {
    using Service::DisplayStartRate;
    // Unlocked at 240 Hz with VRR: the cap (224), not the console's 60 Hz.
    REQUIRE(DisplayStartRate(true, 60, 0, 224, 0) == 224);
    // With AMD frame generation the cap is halved for real frames.
    REQUIRE(DisplayStartRate(true, 60, 0, 112, 0) == 112);
    // The user's limit below the cap wins; no limit and no cap: none.
    REQUIRE(DisplayStartRate(true, 60, 144, 224, 0) == 144);
    REQUIRE(DisplayStartRate(true, 60, 0, 0, 0) == 0);
    // Locked: the console's vsync rate, lowered by the limit and the cap.
    REQUIRE(DisplayStartRate(false, 60, 0, 224, 0) == 60);
    REQUIRE(DisplayStartRate(false, 60, 0, 0, 0) == 60);
    REQUIRE(DisplayStartRate(false, 60, 30, 224, 0) == 30);
    REQUIRE(DisplayStartRate(false, 120, 0, 112, 0) == 112);
    // A 60 fps movie keeps its cadence either way.
    REQUIRE(DisplayStartRate(true, 60, 0, 224, 60) == 180);
    REQUIRE(DisplayStartRate(false, 60, 0, 224, 60) == 60);
}

TEST_CASE("NX2 display vsync starts frames at the display, under the paced rate",
          "[core][nx2_present_pacer]") {
    using Service::DisplayVsyncDeadline;
    // The first frame, or no limit: at once.
    REQUIRE(DisplayVsyncDeadline(std::nullopt, Start, 112) == Start);
    REQUIRE(DisplayVsyncDeadline(Start, Start + 1ms, 0) == Start + 1ms);
    // 112 fps (AMD frame generation's game rate at 240 Hz): 8.928571 ms between starts.
    const auto next = DisplayVsyncDeadline(Start, Start + 4ms, 112);
    REQUIRE(next == Start + 8'928'571ns);
    // A display signal after the interval starts the frame at once.
    REQUIRE(DisplayVsyncDeadline(Start, Start + 10ms, 112) == Start + 10ms);
    // A 30 fps movie cadence (MovieCadenceRate) holds starts 33 ms apart.
    REQUIRE(DisplayVsyncDeadline(Start, Start + 20ms, 30) == Start + 33'333'333ns);
}

TEST_CASE("NX2 vsync timer steps in only when the display stops", "[core][nx2_present_pacer]") {
    using Service::TimerVsyncSignals;
    REQUIRE(TimerVsyncSignals(std::nullopt, Start, OneVblank));
    REQUIRE_FALSE(TimerVsyncSignals(Start, Start + OneVblank, OneVblank));
    REQUIRE_FALSE(TimerVsyncSignals(Start, Start + 2 * OneVblank, OneVblank));
    REQUIRE(TimerVsyncSignals(Start, Start + ThreeVblanks, OneVblank));
}
