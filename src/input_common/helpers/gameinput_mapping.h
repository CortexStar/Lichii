// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "common/common_types.h"

/// GameInput gamepads as SDL joysticks: the SDL engine keeps the device's SDL identity (GUID, port,
/// joystick button/axis/hat numbers from its SDL gamepad mapping), GameInput delivers the input.
/// Platform-free so it is unit-tested.
namespace InputCommon::GameInputMapping {

/// SDL gamepad elements, in SDL_GamepadButton order followed by the axes.
enum class Element : u8 {
    South,
    East,
    West,
    North,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc1,
    LeftX,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
    Count,
};

inline constexpr std::size_t ElementCount = static_cast<std::size_t>(Element::Count);

/// Where an SDL gamepad mapping puts an element on the joystick.
struct Binding {
    enum class Kind : u8 { None, Button, Hat, Axis };
    Kind kind{Kind::None};
    int index{};
    u8 hat_mask{};
    /// Axis inputs: "+a2" / "-a2" use half of the axis, "a2~" inverts it.
    bool half_positive{};
    bool half_negative{};
    bool inverted{};
};

using Bindings = std::array<Binding, ElementCount>;

/// Parses an SDL gamepad mapping string ("guid,name,a:b0,leftx:a0,dpup:h0.1,...").
[[nodiscard]] Bindings ParseSdlMapping(std::string_view mapping);

/// GameInput gamepad state, in GameInput's units: sticks -1..1 with up and right positive,
/// triggers 0..1.
struct GamepadState {
    u32 buttons{};
    float left_trigger{};
    float right_trigger{};
    float left_x{};
    float left_y{};
    float right_x{};
    float right_y{};
    u32 system_buttons{};
};

/// GameInputGamepadButtons and GameInputSystemButtons bits.
namespace Bits {
inline constexpr u32 Menu = 0x1;
inline constexpr u32 View = 0x2;
inline constexpr u32 A = 0x4;
inline constexpr u32 B = 0x8;
inline constexpr u32 X = 0x10;
inline constexpr u32 Y = 0x20;
inline constexpr u32 DpadUp = 0x40;
inline constexpr u32 DpadDown = 0x80;
inline constexpr u32 DpadLeft = 0x100;
inline constexpr u32 DpadRight = 0x200;
inline constexpr u32 LeftShoulder = 0x400;
inline constexpr u32 RightShoulder = 0x800;
inline constexpr u32 LeftThumbstick = 0x1000;
inline constexpr u32 RightThumbstick = 0x2000;
inline constexpr u32 SystemGuide = 0x1;
inline constexpr u32 SystemShare = 0x2;
} // namespace Bits

/// The joystick inputs an SDL joystick with `bindings` would report for `state`, with SDL's
/// joystick axis convention (-1..1, stick down positive before a mapping's inversion).
struct JoystickState {
    struct Button {
        int index;
        bool pressed;
    };
    struct Hat {
        int index;
        u8 value;
    };
    struct Axis {
        int index;
        float value;
    };
    std::vector<Button> buttons;
    std::vector<Hat> hats;
    std::vector<Axis> axes;
};

[[nodiscard]] JoystickState Translate(const Bindings& bindings, const GamepadState& state);

/// GameInputKind and GameInputDeviceFamily values UsesGameInput looks at.
inline constexpr u32 KindGamepad = 0x00040000;
inline constexpr u32 KindSensors = 0x00000040;
inline constexpr int FamilyVirtual = -1;
inline constexpr int FamilyAggregate = 5;

/// A device GameInput reads instead of SDL: a physical gamepad (not GameInput's virtual or
/// aggregate devices) that GameInput reports without sensors (motion stays with SDL, which reads
/// it), and that SDL has a gamepad mapping for (the SDL identity the configuration refers to).
[[nodiscard]] bool UsesGameInput(int family, u32 supported_input, bool has_sdl_mapping);

} // namespace InputCommon::GameInputMapping
