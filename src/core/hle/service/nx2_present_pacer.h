// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace Service {

/// The console's display rate: the vsync event's default rate (Settings frame_rate_limit).
constexpr std::uint32_t ConsoleVsyncHz = 60;

/**
 * Returns the interval the game's present word asks for, or 0 when it does not pace presents.
 *
 * The word only paces while the speed limit is on and the vsync event runs at the console's
 * 60 Hz. With a faster event (a frame rate limit above 60) the event alone paces the game:
 * holding Ocarina's swap interval against the word's 60 Hz as well capped it near 57 fps at a
 * 120 Hz event.
 *
 * @param swap_interval Vblanks per frame from the present word (1-4).
 * @param refresh_rate  Refresh rate from the present word, in Hz.
 * @param vsync_hz      Rate of the vsync event the game starts frames on.
 * @param speed_limit   Speed limit in percent, or 0 when the speed limit is off.
 */
[[nodiscard]] inline std::chrono::nanoseconds PresentWordInterval(std::uint32_t swap_interval,
                                                                  std::uint32_t refresh_rate,
                                                                  std::uint32_t vsync_hz,
                                                                  std::uint32_t speed_limit) {
    if (vsync_hz != ConsoleVsyncHz || speed_limit == 0 || refresh_rate == 0) {
        return std::chrono::nanoseconds{0};
    }
    return std::chrono::nanoseconds{100'000'000'000LL * swap_interval / refresh_rate / speed_limit};
}

/**
 * Returns the interval between paced presents: the vblank the game asked for, lengthened when an
 * explicit FPS limit is below the rate that vblank allows.
 *
 * @param vblank_interval The interval the game's present word asks for, already scaled by the
 *                        speed limit.
 * @param fps_limit       Frames per second to hold, or 0 to leave the game's interval alone.
 * @return The longer of the two intervals.
 */
[[nodiscard]] inline std::chrono::nanoseconds PacedInterval(std::chrono::nanoseconds vblank_interval,
                                                            std::uint32_t fps_limit) {
    if (fps_limit == 0) {
        return vblank_interval;
    }
    const auto limited_interval = std::chrono::nanoseconds{1'000'000'000LL / fps_limit};
    return (std::max)(vblank_interval, limited_interval);
}

/**
 * Returns the FPS limit the pacer holds: the user's limit, lowered to the host's frame cap that
 * keeps presents inside a variable refresh display's range.
 *
 * @param fps_limit Frames per second the user asked for, or 0 for none.
 * @param host_cap  The renderer's frame cap (RendererBase::HostFrameCap), or 0 for none.
 */
[[nodiscard]] constexpr std::uint32_t EffectiveFpsLimit(std::uint32_t fps_limit,
                                                        std::uint32_t host_cap) {
    if (host_cap == 0) {
        return fps_limit;
    }
    return fps_limit == 0 ? host_cap : (std::min)(fps_limit, host_cap);
}

class Nx2PresentPacer {
public:
    using Clock = std::chrono::steady_clock;

    struct Step {
        Clock::time_point deadline;
        bool reset;
    };

    /**
     * Returns the deadline for the present arriving at @p now: the previous deadline plus the
     * interval, re-anchored to @p now once the caller has fallen more than one interval behind.
     *
     * @param now      When the present reached the pacer.
     * @param interval Present interval, normally the value from PacedInterval.
     * @return The deadline to hold until, and whether the pacer re-anchored because it was late.
     */
    Step Advance(Clock::time_point now, std::chrono::nanoseconds interval) {
        const auto deadline = last_deadline ? *last_deadline + interval : now;
        const bool reset = !last_deadline || now > deadline + interval;
        last_deadline = reset ? now : deadline;
        return {*last_deadline, reset};
    }

private:
    std::optional<Clock::time_point> last_deadline;
};

} // namespace Service
