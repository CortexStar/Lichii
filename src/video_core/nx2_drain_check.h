// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2.h"

// EDEN_NX2_DRAIN_CHECK: what the split GPU thread's front would see without some of its waits for
// the back, counted without changing what runs and logged every 10 s by the front:
// - command lists on pages no GPU work had written that did not wait for the compute and copy
//   work sent after the last host-level wait (DmaPusher::Step): how many of them that work made
//   GPU-written, checked on the back at the list's place. It must stay 0;
// - semaphore acquires, and the polls of suspended ones (Puller): whether the value met the
//   acquire before the front drained the split and after, by whether the GPU had released a
//   semaphore at that address before (any engine, RasterizerInterface::Query with a payload);
// - acquires and polls the semaphore view decides (Puller AskView), verified: the front drains
//   and reads the word as before anyway, and counts the decisions the drain contradicts. A pass
//   the drained word does not meet must stay 0; such a pass is corrected (the channel waits), and
//   a wait the drained word meets ends (counted: a drain would have passed or resumed it).
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
    /// Passes the semaphore view decided, by poll * 3 + ViewPass, and those the drained word did
    /// not meet.
    std::array<std::atomic<u64>, 6> view_passes{};
    std::array<std::atomic<u64>, 6> view_wrong{};
    /// Acquires the view suspended and polls it kept waiting (by poll), and of those the ones
    /// the drained word met.
    std::array<std::atomic<u64>, 2> view_waits{};
    std::array<std::atomic<u64>, 2> view_waits_met{};
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

/// How the semaphore view passed an acquire or resumed a poll.
enum class ViewPass : u32 {
    Memory, ///< Met in memory
    Queued, ///< Met by the last release sent to the word, still queued
    Waited, ///< Met after a positioned wait (GpuSplit::WaitForPosition)
};

/// The view passed an acquire (or resumed a poll, `poll`); whether the word read after a drain
/// met it too.
inline void NoteViewPass(bool poll, ViewPass how, bool met_after_drain) {
    const size_t index = (poll ? 3U : 0U) + static_cast<size_t>(how);
    State& state = Get();
    state.view_passes[index].fetch_add(1, std::memory_order_relaxed);
    if (!met_after_drain) {
        state.view_wrong[index].fetch_add(1, std::memory_order_relaxed);
    }
}

/// The view suspended an acquire (or kept a poll waiting, `poll`); whether the word read after
/// a drain met it.
inline void NoteViewWait(bool poll, bool met_after_drain) {
    State& state = Get();
    state.view_waits[poll ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
    if (met_after_drain) {
        state.view_waits_met[poll ? 1 : 0].fetch_add(1, std::memory_order_relaxed);
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
        const size_t base = poll ? 3U : 0U;
        const u64 by_memory = take(state.view_passes[base]);
        const u64 by_queued = take(state.view_passes[base + 1]);
        const u64 waited = take(state.view_passes[base + 2]);
        const u64 wrong = take(state.view_wrong[base]) + take(state.view_wrong[base + 1]) +
                          take(state.view_wrong[base + 2]);
        return fmt::format("{} (by memory {}, by a release still queued {}, after a positioned "
                           "wait {}), not met after the drain {}",
                           by_memory + by_queued + waited, by_memory, by_queued, waited, wrong);
    };
    const auto waits = [&](bool poll) {
        const u64 count = take(state.view_waits[poll ? 1 : 0]);
        const u64 met = take(state.view_waits_met[poll ? 1 : 0]);
        return fmt::format("{} (met after the drain {})", count, met);
    };
    const u64 unwaited = take(state.lists_unwaited);
    const u64 written = take(state.lists_written);
    const std::string acquires_never = group(false, false);
    const std::string acquires_released = group(false, true);
    const std::string polls_never = group(true, false);
    const std::string polls_released = group(true, true);
    const std::string acquire_passes = passes(false);
    const std::string acquire_waits = waits(false);
    const std::string poll_passes = passes(true);
    const std::string poll_waits = waits(true);
    LOG_INFO(HW_GPU,
             "Drain checks in {:.1f} s: command lists that did not wait for writers sent after the "
             "last sync {}, GPU-written by those writers {} | drained acquires at addresses the "
             "GPU never released {}, at ones it released {} | drained polls of suspended acquires "
             "at addresses never released {}, at released ones {} | semaphore view: acquires "
             "passed {}, suspended {}; polls resumed {}, kept waiting {}",
             seconds, unwaited, written, acquires_never, acquires_released, polls_never,
             polls_released, acquire_passes, acquire_waits, poll_passes, poll_waits);
}

} // namespace VideoCommon::Nx2DrainCheck

// EDEN_NX2_ACQUIRE_STATS: how the split's front decides Switch 2 semaphore acquires (Puller), every
// 10 s, without changing what runs: by operation, which of them still wait, at which words (on
// GPU-written pages, by the releases the front sent there), what resumes them and how long they
// waited. Front thread only, but for the writer that latched a waiter (Nx2SemaphoreWaiters).
namespace VideoCommon::Nx2AcquireStats {

// A plain flag: the front checks it at every acquire and release it sends.
inline bool g_enabled = std::getenv("EDEN_NX2_ACQUIRE_STATS") != nullptr;

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_enabled = std::getenv("EDEN_NX2_ACQUIRE_STATS") != nullptr;
}

/// How the front decided an acquire.
enum class Decision : u32 {
    Memory,    ///< Passed: met in memory
    Queued,    ///< Passed: met by the last release sent to the word, still queued
    Waited,    ///< Passed after a positioned wait
    Suspended, ///< The channel waits
    Drained,   ///< The semaphore view doesn't apply: drained as before
    Count,
};

/// Who the front sent releases to a word for (State::noted bits).
enum class Releaser : u32 {
    Puller,        ///< Semaphore methods (WriteLong, SEM_EXECUTE release)
    Report3D,      ///< 3D report semaphores
    ReportCompute, ///< Compute report semaphores
    Copy,          ///< Copy engine semaphores
    Count,
};

/// What resumed a suspended acquire.
enum class Resume : u32 {
    Latch,  ///< A release the back ran latched the waiter (by its Source)
    Memory, ///< Met in memory: the CPU, or a GPU write that latches no waiter
    Queued, ///< Met by the last release sent to the word, still queued
    Drain,  ///< EDEN_NX2_DRAIN_CHECK's drain met it first
    Count,
};

using Source = Tegra::Nx2SemaphoreWaiters::Source;

struct State {
    /// By operation (equal, greater or equal, mask) and Decision.
    std::array<std::array<u64, static_cast<size_t>(Decision::Count)>, 3> acquires{};
    u64 positioned_waits{};
    u64 suspended_gpu_written{}; ///< Words on GPU-written pages
    u64 suspended_flush{};       ///< Of those, words the buffer cache held newer than memory
    std::array<u64, static_cast<size_t>(Releaser::Count)> suspended_noted{};
    u64 suspended_never_noted{}; ///< No release the front sent: QMD releases, the CPU
    std::array<u64, static_cast<size_t>(Resume::Count)> resumes{};
    std::array<u64, static_cast<size_t>(Source::Count)> latches{};
    u64 resumes_after_flush{};
    u64 wait_ns{};
    u64 wait_max_ns{};
    u64 waits_over_1ms{};
    std::unordered_map<u64, u8> noted; ///< Releaser bits by word
    std::chrono::steady_clock::time_point last_report{};
};

inline State& Get() {
    static State state;
    return state;
}

[[nodiscard]] inline size_t OperationIndex(u32 operation) {
    switch (operation) {
    case Tegra::Nx2SemaphoreWaiters::AcquireEqual:
        return 0;
    case Tegra::Nx2SemaphoreWaiters::AcquireGequal:
        return 1;
    default:
        return 2;
    }
}

/// The front sent a release to the word at `device_address`.
inline void NoteRelease(u64 device_address, Releaser releaser) {
    if (!g_enabled) {
        return;
    }
    State& state = Get();
    if (state.noted.size() < 65536) {
        state.noted[device_address & ~u64{3}] |= static_cast<u8>(1U << static_cast<u32>(releaser));
    }
}

/// An acquire of `operation` (Puller GpuSemaphoreOperation) at `device_address`, decided so.
/// Suspended: whether its word is on a GPU-written page, and the buffer cache held it newer.
inline void NoteAcquire(u32 operation, Decision decision, u64 device_address, bool waited,
                        bool gpu_written, bool flush) {
    State& state = Get();
    ++state.acquires[OperationIndex(operation)][static_cast<size_t>(decision)];
    if (waited) {
        ++state.positioned_waits;
    }
    if (decision != Decision::Suspended) {
        return;
    }
    if (gpu_written) {
        ++state.suspended_gpu_written;
    }
    if (flush) {
        ++state.suspended_flush;
    }
    const auto it = state.noted.find(device_address & ~u64{3});
    if (it == state.noted.end()) {
        ++state.suspended_never_noted;
        return;
    }
    for (u32 releaser = 0; releaser < static_cast<u32>(Releaser::Count); ++releaser) {
        if ((it->second >> releaser) & 1) {
            ++state.suspended_noted[releaser];
        }
    }
}

/// A suspended acquire resumed, after `waited`.
inline void NoteResume(Resume how, Source latch_source, bool after_flush,
                       std::chrono::steady_clock::duration waited) {
    State& state = Get();
    ++state.resumes[static_cast<size_t>(how)];
    if (how == Resume::Latch) {
        ++state.latches[static_cast<size_t>(latch_source)];
    }
    if (after_flush) {
        ++state.resumes_after_flush;
    }
    const u64 ns = static_cast<u64>(std::max<std::chrono::nanoseconds::rep>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count()));
    state.wait_ns += ns;
    state.wait_max_ns = std::max(state.wait_max_ns, ns);
    if (ns > 1'000'000) {
        ++state.waits_over_1ms;
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
    const auto by_operation = [&](size_t operation) {
        const auto& counts = state.acquires[operation];
        u64 total = 0;
        for (const u64 count : counts) {
            total += count;
        }
        return fmt::format("{} (passed by memory {}, by a release still queued {}, after a "
                           "positioned wait {}; suspended {}; drained {})",
                           total, counts[0], counts[1], counts[2], counts[3], counts[4]);
    };
    const auto& noted = state.suspended_noted;
    const auto& resumes = state.resumes;
    const auto& latches = state.latches;
    const u64 resumed = resumes[0] + resumes[1] + resumes[2] + resumes[3];
    LOG_INFO(HW_GPU,
             "Acquires in {:.1f} s (EDEN_NX2_ACQUIRE_STATS): equal {}, greater or equal {}, mask "
             "{}; positioned waits {} | suspended at words on GPU-written pages {} (held newer by "
             "the buffer cache {}), with releases sent by the puller {}, 3D reports {}, compute "
             "reports {}, the copy engine {}, none (QMD releases, the CPU) {} | resumed {}: "
             "latched {} (payload releases {}, query results {}, QMD reductions {}, QMD maxima {}, "
             "other {}), by memory {}, by a queued release {}, by the drain check {}, after a "
             "flush {} | waited {:.1f} ms, at most {:.3f} ms, over 1 ms {}",
             seconds, by_operation(0), by_operation(1), by_operation(2), state.positioned_waits,
             state.suspended_gpu_written, state.suspended_flush, noted[0], noted[1], noted[2],
             noted[3], state.suspended_never_noted, resumed, resumes[0], latches[1], latches[2],
             latches[3], latches[4], latches[0], resumes[1], resumes[2], resumes[3],
             state.resumes_after_flush, static_cast<double>(state.wait_ns) / 1e6,
             static_cast<double>(state.wait_max_ns) / 1e6, state.waits_over_1ms);
    auto noted_words = std::move(state.noted);
    state = State{};
    state.noted = std::move(noted_words);
    state.last_report = now;
}

} // namespace VideoCommon::Nx2AcquireStats
