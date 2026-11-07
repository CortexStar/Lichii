// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_DRAIN_CHECK: what the split GPU thread's front would see without some of its waits for
// the back, counted without changing what runs and logged every 10 s by the front:
// - command lists on pages no GPU work had written that did not wait for the compute and copy
//   work sent after the last host-level wait (DmaPusher::Step): how many of them that work made
//   GPU-written, checked on the back at the list's place. It must stay 0;
// - semaphore acquires, and the polls of suspended ones (Puller): whether the value met the
//   acquire before the front drained the split and after, by whether the GPU had released a
//   semaphore at that address before (any engine, RasterizerInterface::Query with a payload).
namespace VideoCommon::Nx2DrainCheck {

// A plain flag: the front checks it at every command list and acquire.
inline bool g_enabled = std::getenv("EDEN_NX2_DRAIN_CHECK") != nullptr;

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_enabled = std::getenv("EDEN_NX2_DRAIN_CHECK") != nullptr;
}

struct State {
    std::atomic<u64> lists_unwaited{};
    std::atomic<u64> lists_written{};
    /// Indexed by poll * 8 + released * 4 + met before the drain * 2 + met after it.
    std::array<std::atomic<u64>, 16> acquires{};
    std::mutex released_mutex;
    std::unordered_set<u64> released; ///< Device addresses of the semaphores the GPU released
    std::chrono::steady_clock::time_point last_report{}; ///< The front's
};

inline State& Get() {
    static State state;
    return state;
}

/// The back runs a semaphore release at `device_address`.
inline void NoteRelease(u64 device_address) {
    State& state = Get();
    std::scoped_lock lock{state.released_mutex};
    state.released.insert(device_address & ~u64{3});
}

[[nodiscard]] inline bool WasReleased(u64 device_address) {
    State& state = Get();
    std::scoped_lock lock{state.released_mutex};
    return state.released.contains(device_address & ~u64{3});
}

/// An acquire, or the poll of a suspended one (`poll`), at a semaphore the GPU had `released`:
/// whether the value met it before the drain and after.
inline void NoteAcquire(bool poll, bool released, bool met_before, bool met_after) {
    const size_t index =
        (poll ? 8U : 0U) + (released ? 4U : 0U) + (met_before ? 2U : 0U) + (met_after ? 1U : 0U);
    Get().acquires[index].fetch_add(1, std::memory_order_relaxed);
}

/// The front fetches a list without waiting for writers still queued after the last sync.
inline void NoteListUnwaited() {
    Get().lists_unwaited.fetch_add(1, std::memory_order_relaxed);
}

/// The back reached that list: whether the work before it made the list's pages GPU-written.
inline void NoteListChecked(bool written) {
    if (written) {
        Get().lists_written.fetch_add(1, std::memory_order_relaxed);
    }
}

/// The front, between commands.
inline void MaybeReport() {
    if (!g_enabled) {
        return;
    }
    State& state = Get();
    const auto now = std::chrono::steady_clock::now();
    if (state.last_report == std::chrono::steady_clock::time_point{}) {
        state.last_report = now;
        return;
    }
    const double seconds = std::chrono::duration<double>(now - state.last_report).count();
    if (seconds < 10.0) {
        return;
    }
    state.last_report = now;
    const auto take = [](std::atomic<u64>& value) {
        return value.exchange(0, std::memory_order_relaxed);
    };
    const auto group = [&](bool poll, bool released) {
        const size_t base = (poll ? 8U : 0U) + (released ? 4U : 0U);
        const u64 neither = take(state.acquires[base]);
        const u64 only_after = take(state.acquires[base + 1]);
        const u64 only_before = take(state.acquires[base + 2]);
        const u64 both = take(state.acquires[base + 3]);
        return fmt::format("{} (met before and after the drain {}, only after {}, only before {}, "
                           "neither {})",
                           neither + only_after + only_before + both, both, only_after, only_before,
                           neither);
    };
    const u64 unwaited = take(state.lists_unwaited);
    const u64 written = take(state.lists_written);
    const std::string acquires_never = group(false, false);
    const std::string acquires_released = group(false, true);
    const std::string polls_never = group(true, false);
    const std::string polls_released = group(true, true);
    LOG_INFO(HW_GPU,
             "Drain checks in {:.1f} s: command lists that did not wait for writers sent after the "
             "last sync {}, GPU-written by those writers {} | acquires at addresses the GPU never "
             "released {}, at ones it released {} | polls of suspended acquires at addresses never "
             "released {}, at released ones {}",
             seconds, unwaited, written, acquires_never, acquires_released, polls_never,
             polls_released);
}

} // namespace VideoCommon::Nx2DrainCheck
