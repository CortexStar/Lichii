// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <charconv>
#include <optional>

#include "input_common/helpers/gameinput_mapping.h"

namespace InputCommon::GameInputMapping {

namespace {

constexpr std::array<std::string_view, ElementCount> ElementNames{
    "a",          "b",           "x",          "y",     "back",   "guide",
    "start",      "leftstick",   "rightstick", "leftshoulder",    "rightshoulder",
    "dpup",       "dpdown",      "dpleft",     "dpright",         "misc1",
    "leftx",      "lefty",       "rightx",     "righty",          "lefttrigger",
    "righttrigger",
};

std::optional<int> ParseInt(std::string_view text) {
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

Binding ParseBinding(std::string_view text) {
    Binding binding{};
    if (text.empty()) {
        return binding;
    }
    if (text.front() == '+' || text.front() == '-') {
        binding.half_positive = text.front() == '+';
        binding.half_negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (!text.empty() && text.back() == '~') {
        binding.inverted = true;
        text.remove_suffix(1);
    }
    if (text.size() < 2) {
        return {};
    }
    const char kind = text.front();
    text.remove_prefix(1);
    if (kind == 'b') {
        if (const auto index = ParseInt(text)) {
            binding.kind = Binding::Kind::Button;
            binding.index = *index;
            return binding;
        }
    } else if (kind == 'a') {
        if (const auto index = ParseInt(text)) {
            binding.kind = Binding::Kind::Axis;
            binding.index = *index;
            return binding;
        }
    } else if (kind == 'h') {
        const std::size_t dot = text.find('.');
        if (dot != std::string_view::npos) {
            const auto index = ParseInt(text.substr(0, dot));
            const auto mask = ParseInt(text.substr(dot + 1));
            if (index && mask && *mask <= 0xF) {
                binding.kind = Binding::Kind::Hat;
                binding.index = *index;
                binding.hat_mask = static_cast<u8>(*mask);
                return binding;
            }
        }
    }
    return {};
}

bool Pressed(const GamepadState& state, Element element) {
    switch (element) {
    case Element::South:
        return state.buttons & Bits::A;
    case Element::East:
        return state.buttons & Bits::B;
    case Element::West:
        return state.buttons & Bits::X;
    case Element::North:
        return state.buttons & Bits::Y;
    case Element::Back:
        return state.buttons & Bits::View;
    case Element::Guide:
        return state.system_buttons & Bits::SystemGuide;
    case Element::Start:
        return state.buttons & Bits::Menu;
    case Element::LeftStick:
        return state.buttons & Bits::LeftThumbstick;
    case Element::RightStick:
        return state.buttons & Bits::RightThumbstick;
    case Element::LeftShoulder:
        return state.buttons & Bits::LeftShoulder;
    case Element::RightShoulder:
        return state.buttons & Bits::RightShoulder;
    case Element::DpadUp:
        return state.buttons & Bits::DpadUp;
    case Element::DpadDown:
        return state.buttons & Bits::DpadDown;
    case Element::DpadLeft:
        return state.buttons & Bits::DpadLeft;
    case Element::DpadRight:
        return state.buttons & Bits::DpadRight;
    case Element::Misc1:
        return state.system_buttons & Bits::SystemShare;
    case Element::LeftTrigger:
        return state.left_trigger >= 0.5f;
    case Element::RightTrigger:
        return state.right_trigger >= 0.5f;
    default:
        return false;
    }
}

/// The element's value on SDL's gamepad scale: sticks -1..1 with down positive, triggers 0..1,
/// buttons 0 or 1.
float GamepadValue(const GamepadState& state, Element element) {
    switch (element) {
    case Element::LeftX:
        return state.left_x;
    case Element::LeftY:
        return -state.left_y;
    case Element::RightX:
        return state.right_x;
    case Element::RightY:
        return -state.right_y;
    case Element::LeftTrigger:
        return state.left_trigger;
    case Element::RightTrigger:
        return state.right_trigger;
    default:
        return Pressed(state, element) ? 1.0f : 0.0f;
    }
}

bool IsTrigger(Element element) {
    return element == Element::LeftTrigger || element == Element::RightTrigger;
}

bool IsStickAxis(Element element) {
    return element == Element::LeftX || element == Element::LeftY || element == Element::RightX ||
           element == Element::RightY;
}

} // Anonymous namespace

Bindings ParseSdlMapping(std::string_view mapping) {
    Bindings bindings{};
    // Skip the GUID and the name.
    for (int field = 0; field < 2; ++field) {
        const std::size_t comma = mapping.find(',');
        if (comma == std::string_view::npos) {
            return bindings;
        }
        mapping.remove_prefix(comma + 1);
    }
    while (!mapping.empty()) {
        const std::size_t comma = mapping.find(',');
        const std::string_view entry = mapping.substr(0, comma);
        mapping.remove_prefix(comma == std::string_view::npos ? mapping.size() : comma + 1);
        const std::size_t colon = entry.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view name = entry.substr(0, colon);
        const auto it = std::ranges::find(ElementNames, name);
        if (it == ElementNames.end()) {
            continue;
        }
        bindings[static_cast<std::size_t>(it - ElementNames.begin())] =
            ParseBinding(entry.substr(colon + 1));
    }
    return bindings;
}

JoystickState Translate(const Bindings& bindings, const GamepadState& state) {
    JoystickState out;
    const auto hat = [&out](int index) -> u8& {
        const auto it = std::ranges::find(out.hats, index, &JoystickState::Hat::index);
        if (it != out.hats.end()) {
            return it->value;
        }
        return out.hats.emplace_back(JoystickState::Hat{index, 0}).value;
    };
    const auto axis = [&out](int index) -> float& {
        const auto it = std::ranges::find(out.axes, index, &JoystickState::Axis::index);
        if (it != out.axes.end()) {
            return it->value;
        }
        return out.axes.emplace_back(JoystickState::Axis{index, 0.0f}).value;
    };
    for (std::size_t i = 0; i < ElementCount; ++i) {
        const Element element = static_cast<Element>(i);
        const Binding& binding = bindings[i];
        switch (binding.kind) {
        case Binding::Kind::None:
            break;
        case Binding::Kind::Button:
            out.buttons.push_back({binding.index, Pressed(state, element)});
            break;
        case Binding::Kind::Hat:
            // Initialise the hat even when this direction is released.
            hat(binding.index) |= Pressed(state, element) ? binding.hat_mask : u8{0};
            break;
        case Binding::Kind::Axis: {
            float value = GamepadValue(state, element);
            if (IsStickAxis(element)) {
                // A full stick axis on a joystick axis; a half one covers only its sign.
                if (binding.half_positive) {
                    value = (std::max)(value, 0.0f);
                } else if (binding.half_negative) {
                    value = (std::min)(value, 0.0f);
                }
            } else if (IsTrigger(element) && !binding.half_positive && !binding.half_negative) {
                // A trigger on a whole joystick axis rests at -1 (XInput, Windows.Gaming.Input).
                value = value * 2.0f - 1.0f;
            } else if (binding.half_negative) {
                // Buttons and half-axis triggers move the axis towards its bound sign.
                value = -value;
            }
            if (binding.inverted) {
                value = -value;
            }
            axis(binding.index) += value;
            break;
        }
        }
    }
    return out;
}

bool UsesGameInput(int family, u32 supported_input, bool has_sdl_mapping) {
    if (family == FamilyVirtual || family == FamilyAggregate) {
        return false;
    }
    return (supported_input & KindGamepad) != 0 && (supported_input & KindSensors) == 0 &&
           has_sdl_mapping;
}

} // namespace InputCommon::GameInputMapping
