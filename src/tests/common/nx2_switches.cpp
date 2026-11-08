// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EXPERIMENT (do not merge): tests of the runtime A/B switch registry.

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_switches.h"

namespace S = Common::Nx2Switches;

TEST_CASE("Nx2Switches parse each variable as its read site did", "[common][nx2_switches]") {
    using S::Switch;
    // value[0] == '1'
    REQUIRE(S::Parse(Switch::NoPark, "1") == 1);
    REQUIRE(S::Parse(Switch::NoPark, "10") == 1);
    REQUIRE(S::Parse(Switch::NoPark, "0") == 0);
    REQUIRE(S::Parse(Switch::NoPark, "") == 0);
    REQUIRE(S::Parse(Switch::NoPark, std::nullopt) == 0);
    // value[0] == '0'
    REQUIRE(S::Parse(Switch::FgHudlessOff, "0") == 1);
    REQUIRE(S::Parse(Switch::FgHudlessOff, "1") == 0);
    REQUIRE(S::Parse(Switch::PresentCadenceOff, "00") == 1);
    // value == "0"
    REQUIRE(S::Parse(Switch::DynStateUnbatched, "0") == 1);
    REQUIRE(S::Parse(Switch::DynStateUnbatched, "00") == 0);
    REQUIRE(S::Parse(Switch::DynStateUnbatched, std::nullopt) == 0);
    // value == "all"
    REQUIRE(S::Parse(Switch::DispatchBarriersAll, "all") == 1);
    REQUIRE(S::Parse(Switch::DispatchBarriersAll, "1") == 0);
    // set at all
    REQUIRE(S::Parse(Switch::NoClaim, "") == 1);
    REQUIRE(S::Parse(Switch::NoClaim, "0") == 1);
    REQUIRE(S::Parse(Switch::NoClaim, std::nullopt) == 0);
    // names
    REQUIRE(S::Parse(Switch::GuestCpuOff, "spin,svcwait") == (S::Spin | S::Svcwait));
    REQUIRE(S::Parse(Switch::GuestCpuOff, "cntpct,hostfn,idle,arb") ==
            (S::Cntpct | S::Hostfn | S::Idle | S::Arb));
    REQUIRE(S::Parse(Switch::GuestCpuOff, "") == 0);
}

TEST_CASE("Nx2Switches names map both ways", "[common][nx2_switches]") {
    for (u32 i = 0; i < static_cast<u32>(S::Switch::Count); ++i) {
        const auto which = static_cast<S::Switch>(i);
        REQUIRE(S::Find(S::Name(which)) == which);
    }
    REQUIRE_FALSE(S::Find("EDEN_NX2_EXP_NO_GPL"));
    REQUIRE_FALSE(S::Find(""));
}

TEST_CASE("Nx2Switches take a requested value at the next latch only", "[common][nx2_switches]") {
    using S::Switch;
    // The test process may have the variable set; start from a known value.
    S::Request(Switch::Evict9000, std::nullopt);
    S::Latch(0);
    REQUIRE(S::Get(Switch::Evict9000) == 0);
    REQUIRE_FALSE(S::Text(Switch::Evict9000));

    REQUIRE(S::Request(Switch::Evict9000, "1") == 1);
    REQUIRE(S::Get(Switch::Evict9000) == 0); // not before the frame point
    REQUIRE(S::Text(Switch::Evict9000) == "1");
    const auto changes = S::Latch(7);
    REQUIRE(changes.size() == 1);
    REQUIRE(changes[0].which == Switch::Evict9000);
    REQUIRE(changes[0].from == 0);
    REQUIRE(changes[0].to == 1);
    REQUIRE(S::Get(Switch::Evict9000) == 1);
    REQUIRE(S::Latch(8).empty()); // nothing pending

    // A request back to the current value changes nothing.
    S::Request(Switch::Evict9000, "x");
    REQUIRE(S::Latch(9).empty());

    S::Request(Switch::Evict9000, std::nullopt);
    REQUIRE(S::Latch(10).size() == 1);
    REQUIRE(S::Get(Switch::Evict9000) == 0);
}

TEST_CASE("Nx2Switches keep the launch value for restart-only parts", "[common][nx2_switches]") {
    using S::Switch;
    const u32 launch = S::AtLaunch(Switch::GuestCpuOff);
    S::Request(Switch::GuestCpuOff, "cntpct,spin");
    S::Latch(0);
    REQUIRE(S::Get(Switch::GuestCpuOff) == (S::Cntpct | S::Spin));
    REQUIRE(S::AtLaunch(Switch::GuestCpuOff) == launch);
    S::Request(Switch::GuestCpuOff, std::nullopt);
    S::Latch(1);
    REQUIRE(S::Get(Switch::GuestCpuOff) == 0);
}
