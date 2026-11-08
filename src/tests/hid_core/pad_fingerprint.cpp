// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "hid_core/resources/npad/npad.h"

using Service::HID::NPadGenericState;
using Service::HID::PadFingerprint;

TEST_CASE("The pad fingerprint changes with what the guest reads", "[hid][npad]") {
    const NPadGenericState idle{};
    REQUIRE(PadFingerprint(idle) == 0);

    NPadGenericState state{};
    state.sampling_number = 99; // not input: same fingerprint
    REQUIRE(PadFingerprint(state) == PadFingerprint(idle));

    state.npad_buttons.raw = Core::HID::NpadButton::A;
    const u64 a = PadFingerprint(state);
    REQUIRE(a != PadFingerprint(idle));

    NPadGenericState stick{};
    stick.l_stick = {.x = 0, .y = 32767};
    NPadGenericState other{};
    other.r_stick = {.x = 0, .y = 32767};
    REQUIRE(PadFingerprint(stick) != PadFingerprint(idle));
    REQUIRE(PadFingerprint(stick) != PadFingerprint(other)); // which stick matters
    NPadGenericState stick2 = stick;
    REQUIRE(PadFingerprint(stick2) == PadFingerprint(stick));
}
