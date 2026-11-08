// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

/// Numbers the guest's frames from where the guest starts each one (its present returned, it
/// samples input next) to where the host presents it, for Anti-Lag 2 (whose input and present
/// stages share a frame index) and Lichii's own input-to-present latency.
namespace VideoCore::GuestFrameTiming {

struct Mark {
    u64 id{};       ///< 0 before the first boundary
    u64 start_ns{}; ///< steady clock
};

/// The guest begins its next frame now. Returns that frame.
Mark BeginFrame();

/// The frame the guest is rendering now.
[[nodiscard]] Mark Current();

/// The host presents frame `mark` now: records start-to-present.
void RecordPresent(const Mark& mark);

struct LatencyStats {
    u64 samples{};
    double mean_ms{};
    double p50_ms{};
    double p99_ms{};
    double max_ms{};
};

/// Start-to-present over the last (up to) 1024 presents.
[[nodiscard]] LatencyStats Recent();

/// Pure statistics over a set of samples (unit-tested).
[[nodiscard]] LatencyStats Summarize(std::array<double, 1024> samples, u64 count);

} // namespace VideoCore::GuestFrameTiming
