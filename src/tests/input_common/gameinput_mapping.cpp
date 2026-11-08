// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "input_common/helpers/gameinput_mapping.h"

using namespace InputCommon::GameInputMapping;
using Catch::Matchers::WithinAbs;

namespace {

// SDL 3.4's mapping for XInput controllers (SDL_gamepad_db.h, "xinput").
constexpr std::string_view XInputMapping =
    "xinput,*,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b10,"
    "leftshoulder:b4,leftstick:b8,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,"
    "rightstick:b9,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,";

const Binding& Of(const Bindings& bindings, Element element) {
    return bindings[static_cast<std::size_t>(element)];
}

bool ButtonPressed(const JoystickState& state, int index) {
    const auto it = std::ranges::find(state.buttons, index, &JoystickState::Button::index);
    REQUIRE(it != state.buttons.end());
    return it->pressed;
}

float AxisValue(const JoystickState& state, int index) {
    const auto it = std::ranges::find(state.axes, index, &JoystickState::Axis::index);
    REQUIRE(it != state.axes.end());
    return it->value;
}

u8 HatValue(const JoystickState& state, int index) {
    const auto it = std::ranges::find(state.hats, index, &JoystickState::Hat::index);
    REQUIRE(it != state.hats.end());
    return it->value;
}

} // Anonymous namespace

TEST_CASE("SDL gamepad mappings parse into joystick bindings", "[input][gameinput]") {
    const Bindings bindings = ParseSdlMapping(XInputMapping);
    CHECK(Of(bindings, Element::South).kind == Binding::Kind::Button);
    CHECK(Of(bindings, Element::South).index == 0);
    CHECK(Of(bindings, Element::Guide).index == 10);
    CHECK(Of(bindings, Element::DpadLeft).kind == Binding::Kind::Hat);
    CHECK(Of(bindings, Element::DpadLeft).hat_mask == 8);
    CHECK(Of(bindings, Element::RightTrigger).kind == Binding::Kind::Axis);
    CHECK(Of(bindings, Element::RightTrigger).index == 5);
    CHECK(Of(bindings, Element::Misc1).kind == Binding::Kind::None);

    const Bindings modified = ParseSdlMapping("guid,name,lefty:a1~,lefttrigger:+a2,dpup:-a5,x:q3,");
    CHECK(Of(modified, Element::LeftY).inverted);
    CHECK(Of(modified, Element::LeftTrigger).half_positive);
    CHECK(Of(modified, Element::DpadUp).half_negative);
    CHECK(Of(modified, Element::West).kind == Binding::Kind::None);
    CHECK(Of(ParseSdlMapping("garbage"), Element::South).kind == Binding::Kind::None);
}

TEST_CASE("GameInput readings become the joystick inputs SDL would report", "[input][gameinput]") {
    const Bindings bindings = ParseSdlMapping(XInputMapping);

    const JoystickState rest = Translate(bindings, {});
    CHECK_FALSE(ButtonPressed(rest, 0));
    CHECK(HatValue(rest, 0) == 0);
    // XInput triggers rest at -1 on their joystick axes.
    CHECK_THAT(AxisValue(rest, 2), WithinAbs(-1.0, 1e-6));
    CHECK_THAT(AxisValue(rest, 0), WithinAbs(0.0, 1e-6));

    GamepadState state{};
    state.buttons = Bits::A | Bits::DpadUp | Bits::DpadLeft | Bits::Menu;
    state.system_buttons = Bits::SystemGuide;
    state.left_y = 1.0f; // Up in GameInput, down positive in SDL.
    state.right_x = 0.5f;
    state.right_trigger = 1.0f;
    const JoystickState pressed = Translate(bindings, state);
    CHECK(ButtonPressed(pressed, 0));
    CHECK_FALSE(ButtonPressed(pressed, 1));
    CHECK(ButtonPressed(pressed, 7));
    CHECK(ButtonPressed(pressed, 10));
    CHECK(HatValue(pressed, 0) == (1 | 8));
    CHECK_THAT(AxisValue(pressed, 1), WithinAbs(-1.0, 1e-6));
    CHECK_THAT(AxisValue(pressed, 3), WithinAbs(0.5, 1e-6));
    CHECK_THAT(AxisValue(pressed, 5), WithinAbs(1.0, 1e-6));
}

TEST_CASE("Half and inverted axis bindings", "[input][gameinput]") {
    const Bindings bindings = ParseSdlMapping(
        "guid,name,lefty:a1~,lefttrigger:+a2,righttrigger:-a2,dpup:-a5,dpdown:+a5,b:b1,");
    GamepadState state{};
    state.left_y = 0.25f;
    state.left_trigger = 0.5f;
    state.right_trigger = 0.25f;
    state.buttons = Bits::DpadUp;
    const JoystickState out = Translate(bindings, state);
    // Inverted: SDL's lefty (down positive) is -0.25, the joystick axis the opposite.
    CHECK_THAT(AxisValue(out, 1), WithinAbs(0.25, 1e-6));
    // Two triggers sharing one axis, one per half.
    CHECK_THAT(AxisValue(out, 2), WithinAbs(0.25, 1e-6));
    // A d-pad on the halves of one axis.
    CHECK_THAT(AxisValue(out, 5), WithinAbs(-1.0, 1e-6));
}

TEST_CASE("GameInput reads only physical gamepads without sensors that SDL can map",
          "[input][gameinput]") {
    CHECK(UsesGameInput(1, KindGamepad, true));
    CHECK(UsesGameInput(3, KindGamepad | 0x0E, true));
    // Motion stays with SDL.
    CHECK_FALSE(UsesGameInput(3, KindGamepad | KindSensors, true));
    // No SDL identity for the configuration to refer to.
    CHECK_FALSE(UsesGameInput(1, KindGamepad, false));
    CHECK_FALSE(UsesGameInput(3, 0x0E, true));
    CHECK_FALSE(UsesGameInput(FamilyVirtual, KindGamepad, true));
    CHECK_FALSE(UsesGameInput(FamilyAggregate, KindGamepad, true));
}
