// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "common/common_types.h"

/// Where boot and load time goes: one log line per boot phase, one when the first guest frame is
/// presented, and one per load (a stretch of slow guest frames) with the host work done during it.
namespace Core::LoadTiming {

using Clock = std::chrono::steady_clock;

/// Host work that loads consist of. Load lines report the difference of two snapshots.
struct Counters {
    u64 file_reads{};
    u64 file_bytes{};
    u64 file_ns{};
    u64 service_calls{};
    u64 service_ns{};
    u64 jit_blocks{};
    u64 jit_ns{};
    u64 jit_cache_clears{};

    Counters operator-(const Counters& rhs) const;
};

/// A guest read of game or save data served from a host file (fsp IStorage and IFile).
void CountFileRead(u64 bytes, Clock::duration host_time);
/// One service request, with the time its handler ran.
void CountServiceCall(Clock::duration handler_time);
Counters ReadCounters();

struct LoadWindow {
    Clock::time_point begin; ///< The last regular frame before the first slow one.
    Clock::time_point end;   ///< The last slow frame.
    u64 frames{};            ///< Frames presented after begin, up to and including end.
    Counters work;           ///< Work done between begin and end.
};

/// Finds loads in the guest frame times. A frame that comes SlowFrame or more after the previous
/// one starts a load (games present loading frames at a few fps); RegularFrames frames in a row
/// below that end it. Loads shorter than MinimumLoad are not reported.
class LoadWindowTracker {
public:
    static constexpr Clock::duration SlowFrame = std::chrono::milliseconds{100};
    static constexpr u32 RegularFrames = 30;
    static constexpr Clock::duration MinimumLoad = std::chrono::seconds{1};

    /// Returns the load that this frame ends, if any.
    std::optional<LoadWindow> OnFrame(Clock::time_point now, const Counters& counters);

private:
    bool has_frame{};
    bool in_load{};
    Clock::time_point last_frame{};
    Counters last_counters{};
    u32 regular_run{};
    LoadWindow window{};
    Counters begin_counters{};
};

/// Starts the boot clock and clears the load tracker (System::Load).
void BeginBoot();
/// Logs "Load phase <name>: <ms> ms" for a phase that began at start.
void LogPhase(std::string_view name, Clock::time_point start);
/// Called for every frame the guest presents.
void OnGuestFrame();

class ScopedPhase {
public:
    explicit ScopedPhase(std::string_view name_) : name{name_}, start{Clock::now()} {}
    ~ScopedPhase() {
        LogPhase(name, start);
    }

    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;

private:
    std::string_view name;
    Clock::time_point start;
};

} // namespace Core::LoadTiming
