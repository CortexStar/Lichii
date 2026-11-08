// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <optional>

#include "common/common_types.h"

/// Starts the game's frames from the host's display instead of a fixed timer: the presenter
/// reports each frame that reached the display (present_wait2 on Lichii's swapchain, AMD's frame
/// generation swapchain taking a present), and the nx2:vi2 session signals its vsync event then.
/// Anti-Lag 2's input stage runs right before that signal. The fixed timer only steps in while
/// the display reports nothing (TimerVsyncSignals). EDEN_NX2_HOST_VSYNC=0 keeps the timer.
namespace VideoCore::HostVsync {

using Clock = std::chrono::steady_clock;

/// Display-driven vsync is not switched off.
[[nodiscard]] bool Enabled();

/// The game's frame starter (the nx2:vi2 session's vsync events); an empty function removes it.
void SetListener(std::function<void()> listener);

/// The input stage run before each start with the frame about to start (Anti-Lag 2). With
/// `limits`, it holds the frame rate itself (Anti-Lag 2's maxFPS) and gets that rate.
void SetInputStage(std::function<void(u64 next_frame, u32 max_fps)> stage, bool limits);

/// Frames per second to start at most (the pacer's FPS limit with the VRR cap and the movie
/// cadence), 0 for no limit.
void SetRate(u32 rate);

/// The presenter: a frame reached the display. Starts the game's next frame, after the rate
/// allows it (this thread waits for that) and after the input stage.
void Displayed();

/// When the display last started a frame, if it ever did.
[[nodiscard]] std::optional<Clock::time_point> LastStart();

} // namespace VideoCore::HostVsync
