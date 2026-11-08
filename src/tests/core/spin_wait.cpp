// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/spin_wait.h"

using Core::DecodeSpinWatch;
using Core::SpinAction;
using Core::SpinEpisode;
using Core::SpinWatch;

namespace {
struct Tally {
    u32 pauses{};
    u32 waits{};
    u32 guest_yields{};
    u32 host_yields{};
    SpinAction first_after_gap{};
    SpinAction first_ever{};
    SpinAction first_past_pauses{};
};

constexpr Tally RunEpisode() {
    SpinEpisode episode;
    constexpr u64 Gap = 100;
    Tally tally;
    u64 now = 1000;
    for (u32 i = 0; i < SpinEpisode::PauseYields + SpinEpisode::HostYieldPeriod; ++i) {
        const SpinAction action = episode.OnYield(now += 10, Gap);
        if (i == SpinEpisode::PauseYields) {
            tally.first_past_pauses = action;
        }
        switch (action) {
        case SpinAction::Pause:
            ++tally.pauses;
            break;
        case SpinAction::Wait:
            ++tally.waits;
            break;
        case SpinAction::YieldGuestCore:
            ++tally.guest_yields;
            break;
        case SpinAction::YieldHostCpu:
            ++tally.host_yields;
            break;
        }
    }
    // A YIELD after more than the gap starts a new episode.
    tally.first_after_gap = episode.OnYield(now + Gap + 1, Gap);
    // The first YIELD ever starts an episode, whatever the clock reads.
    SpinEpisode fresh;
    tally.first_ever = fresh.OnYield(0, Gap);
    return tally;
}
} // namespace

TEST_CASE("SpinEpisode escalates a long spin and restarts after a gap", "[spin_wait]") {
    constexpr Tally tally = RunEpisode();
    STATIC_CHECK(tally.pauses == SpinEpisode::PauseYields);
    STATIC_CHECK(tally.first_past_pauses == SpinAction::YieldGuestCore);
    STATIC_CHECK(tally.guest_yields ==
                 SpinEpisode::HostYieldPeriod / SpinEpisode::GuestYieldPeriod);
    STATIC_CHECK(tally.host_yields == 1);
    STATIC_CHECK(tally.waits ==
                 SpinEpisode::HostYieldPeriod - tally.guest_yields - tally.host_yields);
    STATIC_CHECK(tally.first_after_gap == SpinAction::Pause);
    STATIC_CHECK(tally.first_ever == SpinAction::Pause);
}

TEST_CASE("DecodeSpinWatch finds the word a spin loop polls after YIELD", "[spin_wait]") {
    // casal w9, w8, [x10]
    STATIC_CHECK(DecodeSpinWatch(std::array<u32, 1>{0x88E9FD48}) == SpinWatch{10, 0, 4});
    // ldar w9, [x9]
    STATIC_CHECK(DecodeSpinWatch(std::array<u32, 1>{0x88DFFD29}) == SpinWatch{9, 0, 4});
    // ldapr x9, [x9]; dmb ishld
    STATIC_CHECK(DecodeSpinWatch(std::array<u32, 2>{0xF8BFC129, 0xD50339BF}) ==
                 SpinWatch{9, 0, 8});
    // add x10, x8, #0x8; casal w9, w25, [x10]
    STATIC_CHECK(DecodeSpinWatch(std::array<u32, 2>{0x9100210A, 0x88E9FD59}) ==
                 SpinWatch{8, 8, 4});
    // dmb ishst; ldr x8, [x0, #0x48]
    STATIC_CHECK(DecodeSpinWatch(std::array<u32, 2>{0xD5033ABF, 0xF9402408}) ==
                 SpinWatch{0, 0x48, 8});
    // ldur w8, [x1, #-4]
    STATIC_CHECK(DecodeSpinWatch(std::array<u32, 1>{0xB85FC028}) == SpinWatch{1, -4, 4});
    // ldadd w8, w9, [sp]
    STATIC_CHECK(DecodeSpinWatch(std::array<u32, 1>{0xB82803E9}) == SpinWatch{31, 0, 4});
    // A backoff loop polls nothing: cmp w11, #1; b.hi
    STATIC_CHECK_FALSE(DecodeSpinWatch(std::array<u32, 2>{0x7100057F, 0x54FFFF88}).has_value());
    // Stores read nothing: stlr wzr, [x8]
    STATIC_CHECK_FALSE(DecodeSpinWatch(std::array<u32, 1>{0x889FFD1F}).has_value());
    // An add to SP ends the pattern.
    STATIC_CHECK_FALSE(DecodeSpinWatch(std::array<u32, 1>{0x910043FF}).has_value());
    STATIC_CHECK_FALSE(DecodeSpinWatch(std::array<u32, 0>{}).has_value());
}
