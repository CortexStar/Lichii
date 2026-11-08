// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
namespace Frontend {
class EmuWindow;
}
} // namespace Core

namespace VideoCommon::GpuRecord {

/// How a replay keeps the guest's order between its own steps and the GPU's work.
enum class ReplaySync {
    /// Before each step, wait for the GPU progress (guest syncpoint values) the guest had seen
    /// when it took that step in the recording: the guest only reused memory, and only
    /// presented, once the GPU work it waited for was done.
    Observed,
    /// No waits: the guest as if infinitely fast. Exact only while the frames still match.
    NoWait,
};

struct ReplayOptions {
    std::filesystem::path path;
    /// Where the report, the recorded frames and the replayed ones go.
    std::filesystem::path out_dir;
    /// Plays the stream this many times; each later loop starts from the recorded state again
    /// with the caches warm (timing). Only the first one captures frames.
    u32 loops = 1;
    ReplaySync sync = ReplaySync::Observed;
    /// Captures a frame wherever the recording has one (the exactness check).
    bool screenshots = true;
    /// Loads the program's disk pipeline cache first, as a boot does.
    bool disk_cache = true;
};

struct FrameTiming {
    u32 loop;
    u64 index;        ///< Composite index in the stream
    double ms;        ///< Since the previous composite was shown (0 for the first)
    double thread_ms; ///< CPU time of the thread showing composites over the same span
};

struct ReplayReport {
    bool ok = false;
    std::string error;
    std::string metadata;
    u64 events = 0;
    u64 submits = 0;
    u64 composites = 0;
    u64 frames_compared = 0;
    double setup_ms = 0.0;
    std::vector<double> loop_ms;
    std::vector<FrameTiming> frames;
    std::vector<std::pair<std::string, u64>> verify_differences;
    std::vector<std::pair<std::string, u64>> verify_checks;
};

/// Replays a recording (record_format.h) through `system`'s real GPU emulation: boots no guest,
/// recreates the recorded GPU state, then feeds the stream as the guest did. Writes
/// `report.json`, `recorded-<n>.png` and `replayed-<n>.png` into out_dir.
[[nodiscard]] ReplayReport RunReplay(Core::System& system, Core::Frontend::EmuWindow& window,
                                     const ReplayOptions& options);

/// The report as JSON.
[[nodiscard]] std::string ReportJson(const ReplayReport& report);

/// Mean, p50, p99 and max of the frame times of loops >= first_loop.
struct FrameStats {
    u64 frames = 0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    double fps = 0.0;
};
[[nodiscard]] FrameStats SummarizeFrames(const std::vector<FrameTiming>& frames, u32 first_loop);

} // namespace VideoCommon::GpuRecord
