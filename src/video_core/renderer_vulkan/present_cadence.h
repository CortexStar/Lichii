// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

#include "common/common_types.h"

namespace Vulkan {

/// Ticks per second for the present cadence, 0 for none (every frame is presented when ready).
/// @param frame_cap  The swapchain's VRR frame cap (PresentPolicy::frame_cap), 0 without VRR.
/// @param guest_rate The rate the guest's presents are paced at (a movie's multiple, a vblank
///                   interval, the FPS limit), 0 when they are not paced.
constexpr u32 PresentCadenceRate(u32 frame_cap, u32 guest_rate) {
    if (frame_cap == 0) {
        return 0;
    }
    return guest_rate == 0 ? frame_cap : (std::min)(frame_cap, guest_rate);
}

/**
 * The present ticks of a variable refresh display. An OLED's brightness follows its refresh
 * interval, so every change of the interval between presents is a visible brightness step. The
 * present thread presents once per tick, a new frame when one is ready and the last one again
 * otherwise, so loads, stalls and uneven frame times leave the refresh interval alone.
 *
 * A new rate is reached gradually: the interval changes by at most MaxStep per tick. That is the
 * VESA Adaptive-Sync Display CTS's low-variability case (its zigzag sweep moves 1 Hz per frame,
 * 0.45 % at 224 Hz).
 */
class PresentCadence {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr double MaxStep = 0.005;

    /**
     * Returns the next tick after the previous one, at @p rate_hz once the slew allows it.
     * Re-anchors to @p now when the caller is more than one interval late (the thread was not
     * scheduled), instead of presenting a burst to catch up.
     */
    Clock::time_point Next(Clock::time_point now, u32 rate_hz) {
        const double target = 1e9 / static_cast<double>((std::max)(rate_hz, u32{1}));
        interval_ns = interval_ns == 0.0 ? target
                                         : std::clamp(target, interval_ns * (1.0 - MaxStep),
                                                      interval_ns * (1.0 + MaxStep));
        const auto interval = Interval();
        Clock::time_point tick = last_tick ? *last_tick + interval : now;
        if (now > tick + interval) {
            tick = now;
        }
        last_tick = tick;
        return tick;
    }

    [[nodiscard]] std::chrono::nanoseconds Interval() const {
        return std::chrono::nanoseconds{static_cast<s64>(interval_ns)};
    }

    /// Forgets the ticks: the next one is at once, at the requested rate.
    void Reset() {
        last_tick.reset();
        interval_ns = 0.0;
    }

private:
    std::optional<Clock::time_point> last_tick;
    double interval_ns{};
};

} // namespace Vulkan
