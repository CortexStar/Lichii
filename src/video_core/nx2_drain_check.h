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
//   semaphore at that address before (any engine, RasterizerInterface::Query with a payload);
// - acquires and polls decided without the drain (Puller CheckWithoutDrain), verified: the front
//   drains and reads the word as before anyway, and counts the decisions the drain contradicts.
//   A pass the drained word does not meet must stay 0; such a decision is corrected.
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
    /// Passes without the drain, by poll * 2 + met by a release still queued, and those the
    /// drained word did not meet.
    std::array<std::atomic<u64>, 4> passes{};
    std::array<std::atomic<u64>, 4> wrong_passes{};
    std::atomic<u64> polls_kept{};     ///< Polls that kept waiting without the drain
    std::atomic<u64> polls_kept_met{}; ///< Of those, met after the drain
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

/// An acquire (or a poll, `poll`) passed without the drain, met by a release still queued
/// (`queued`) or by memory; whether the word read after a drain met it too.
inline void NotePass(bool poll, bool queued, bool met_after_drain) {
    const size_t index = (poll ? 2U : 0U) + (queued ? 1U : 0U);
    State& state = Get();
    state.passes[index].fetch_add(1, std::memory_order_relaxed);
    if (!met_after_drain) {
        state.wrong_passes[index].fetch_add(1, std::memory_order_relaxed);
    }
}

/// A poll kept waiting without the drain; whether the word read after a drain met it.
inline void NoteKeptWaiting(bool met_after_drain) {
    State& state = Get();
    state.polls_kept.fetch_add(1, std::memory_order_relaxed);
    if (met_after_drain) {
        state.polls_kept_met.fetch_add(1, std::memory_order_relaxed);
    }
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
    const auto passes = [&](bool poll) {
        const size_t base = poll ? 2U : 0U;
        const u64 by_memory = take(state.passes[base]);
        const u64 by_queued = take(state.passes[base + 1]);
        const u64 wrong = take(state.wrong_passes[base]) + take(state.wrong_passes[base + 1]);
        return fmt::format("{} (by a release still queued {}), not met after the drain {}",
                           by_memory + by_queued, by_queued, wrong);
    };
    const u64 unwaited = take(state.lists_unwaited);
    const u64 written = take(state.lists_written);
    const std::string acquires_never = group(false, false);
    const std::string acquires_released = group(false, true);
    const std::string polls_never = group(true, false);
    const std::string polls_released = group(true, true);
    const std::string acquire_passes = passes(false);
    const std::string poll_passes = passes(true);
    const u64 kept = take(state.polls_kept);
    const u64 kept_met = take(state.polls_kept_met);
    LOG_INFO(HW_GPU,
             "Drain checks in {:.1f} s: command lists that did not wait for writers sent after the "
             "last sync {}, GPU-written by those writers {} | acquires at addresses the GPU never "
             "released {}, at ones it released {} | polls of suspended acquires at addresses never "
             "released {}, at released ones {} | without the drain: acquires passed {}; polls "
             "resumed {}, kept waiting {} (met after the drain {})",
             seconds, unwaited, written, acquires_never, acquires_released, polls_never,
             polls_released, acquire_passes, poll_passes, kept, kept_met);
}

} // namespace VideoCommon::Nx2DrainCheck
