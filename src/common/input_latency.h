// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <optional>

#include "common/common_types.h"

/// Input-to-present latency measured inside Lichii, independent of PresentMon: the host time of
/// a button change travels with the first guest frame that starts after it (the frame that
/// samples it) to that frame's host present.
namespace Common::InputLatency {

/// The decisions, without clocks or globals (unit-tested).
class Tracker {
public:
    /// A button changed at `ns`. A change that waits for a frame keeps the earliest time.
    void Changed(u64 ns);

    /// The guest starts a frame: returns the change it samples, 0 when nothing changed since the
    /// previous start.
    [[nodiscard]] u64 FrameStart();

    /// The host presents a frame that sampled `input_ns` at `now_ns`: the latency to record,
    /// none for a frame without a change or for a change an earlier present already showed.
    /// Called from one thread.
    [[nodiscard]] std::optional<u64> Present(u64 input_ns, u64 now_ns);

private:
    std::atomic<u64> pending{};
    u64 last_presented{};
};

struct Stats {
    u64 samples{};
    double mean_ms{};
    double p50_ms{};
    double p99_ms{};
    double max_ms{};
};

inline constexpr std::size_t MaxSamples = 256;

/// Statistics over the first `count` (up to MaxSamples) of `samples_ms` (unit-tested).
[[nodiscard]] Stats Summarize(std::array<double, MaxSamples> samples_ms, u64 count);

/// A button of an emulated controller changed now (host steady clock).
void Changed();

/// The guest starts a frame now.
void FrameStart();

/// The change the frame the guest renders now sampled (for its present), 0 when none.
[[nodiscard]] u64 CurrentFrameInput();

/// The host presents a frame that sampled `input_ns` now. Logs the statistics every 10 seconds
/// with new samples.
void RecordPresent(u64 input_ns);

/// Input-to-present over the last (up to) MaxSamples button changes.
[[nodiscard]] Stats Recent();

} // namespace Common::InputLatency
