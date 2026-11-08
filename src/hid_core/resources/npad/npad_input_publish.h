// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

namespace Core::HID {
enum class ControllerTriggerType;
}

namespace Service::HID {

/// Publishes a host input change to the npad shared memory right away instead of at the next
/// 1 ms update. Changes that arrive before the publish runs share it.
class NpadInputPublish {
public:
    /// Changes the game reads from the npad LIFOs. Motion has its own update and the rest are
    /// connection and device state.
    [[nodiscard]] static bool Publishes(Core::HID::ControllerTriggerType type);

    /// An input changed. True when the caller schedules the publish; false when one is already
    /// pending and will read this change.
    [[nodiscard]] bool Request() {
        return !pending.exchange(true, std::memory_order_acq_rel);
    }

    /// The publish starts reading the controllers: a change from now on needs another one.
    void Begin() {
        pending.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> pending{};
};

} // namespace Service::HID
