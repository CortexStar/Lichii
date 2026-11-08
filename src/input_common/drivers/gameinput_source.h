// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "common/common_types.h"
#include "input_common/helpers/gameinput_mapping.h"

namespace InputCommon {

/// Gamepads through Microsoft GameInput (Windows): GameInput calls back with each new reading as
/// the device reports it, so a gamepad reaches the emulated controller without waiting for an SDL
/// pump. Loaded at run time from GameInputRedist.dll (the GameInput redistributable) or a
/// GameInput.dll that exports GameInputInitialize; Create returns nullptr when neither is there
/// or GameInput refuses API version 3, and SDL then reads every controller.
class GameInputSource {
public:
    /// An opaque device handle, valid from `connected` until `disconnected` returns.
    using Pad = void*;

    struct Device {
        u16 vendor_id;
        u16 product_id;
        int family;
        u32 supported_input;
        std::string name;
    };

    struct Callbacks {
        std::function<void(Pad, const Device&)> connected;
        std::function<void(Pad)> disconnected;
        /// On GameInput's thread, for every new reading of a connected pad.
        std::function<void(Pad, const GameInputMapping::GamepadState&)> state;
    };

    /// Connected pads are reported before Create returns.
    [[nodiscard]] static std::unique_ptr<GameInputSource> Create(Callbacks callbacks);

    ~GameInputSource();

    /// Rumble motor speeds 0..1.
    void Rumble(Pad pad, float low_frequency, float high_frequency);

    struct Impl;

private:
    explicit GameInputSource(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl;
};

} // namespace InputCommon
