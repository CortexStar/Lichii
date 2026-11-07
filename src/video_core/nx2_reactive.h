// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

// Reactive flushing of GPU-written buffer data on Switch 2 titles. Their GPU writes are not
// copied back at fences (BufferCache::AsyncBufferDownloads): a guest CPU read of a GPU-written
// page traps, drains the GPU thread and waits for the host GPU to go idle (Scheduler::Finish)
// before the data is copied back (GPU::OnCPURead). Ocarina takes ~160 of these a second, ~5 a
// frame, mostly on a few hundred pages it reads every frame.
namespace VideoCommon::Nx2Reactive {

inline bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1';
}

/// EDEN_NX2_REACTIVE_PREFETCH=1: GPU writes over pages the guest CPU has read GPU data from
/// (BufferCache::cpu_read_ranges) are also copied back at the next fence; their traps stay armed.
/// A read after the copy reaches guest memory goes on without a stall, an earlier one flushes as
/// before. Off by default: like every fence download it is written when its fence pops, so a
/// guest CPU write to the same bytes between the GPU write and the next collection of CPU writes
/// can be overwritten by the GPU's bytes.
inline bool Prefetch() {
    static const bool enabled = EnvFlag("EDEN_NX2_REACTIVE_PREFETCH");
    return enabled;
}

/// EDEN_NX2_REACTIVE_PREFLUSH=1: upstream yuzu's reactive flushing on Switch 2 titles too: the
/// first read of a GPU-written page flushes it and marks it Preflushable, so later GPU writes
/// over it are copied back at fences and its reads never stall again. A heuristic, off by
/// default: fast GPU fences signal syncpoints before the host GPU has run the work, so a read
/// right after a syncpoint wait can see the previous copy (one GPU round trip late). Fine for
/// data read a frame later (auto-exposure), wrong for data needed back in the same frame.
inline bool Preflush() {
    static const bool enabled = EnvFlag("EDEN_NX2_REACTIVE_PREFLUSH");
    return enabled;
}

/// EDEN_NX2_REACTIVE_DIAG=1: every second, a summary of the guest CPU's reads of cached GPU
/// memory that asked the caches for a flush area (GPU::OnCPURead): how many found GPU-written
/// data and stalled, the time the reading threads waited, and the regions behind them.
inline const bool g_diag = EnvFlag("EDEN_NX2_REACTIVE_DIAG");

enum class Kind : u8 {
    Clean,  ///< Neither cache holds GPU-written data there
    Buffer, ///< Buffer cache area
    Image,  ///< Texture cache area (a GPU-modified image)
};

struct Region {
    u64 end{};
    Kind kind{Kind::Clean};
    u64 lookups{};
    u64 stalls{};
    u64 stall_ns{};
};

struct State {
    std::mutex mutex;
    std::unordered_map<u64, Region> regions; ///< By start address of the flush area
    std::chrono::steady_clock::time_point last{std::chrono::steady_clock::now()};
    u64 lookups{};
    u64 buffer_lookups{};
    u64 image_lookups{};
    u64 stalls{};
    u64 stall_ns{};
    u64 prefetch_writes{};
    u64 prefetch_bytes{};
    u64 preflush_marks{};
};

inline State& Get() {
    static State state;
    return state;
}

inline const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::Buffer:
        return "buffer";
    case Kind::Image:
        return "image";
    case Kind::Clean:
        break;
    }
    return "clean";
}

/// Logs and resets the counters once a second. Called with the state's mutex held.
inline void MaybeReportLocked(State& state) {
    const auto now = std::chrono::steady_clock::now();
    if (now - state.last < std::chrono::seconds{1}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - state.last).count();
    state.last = now;
    LOG_INFO(HW_GPU,
             "Reactive diag {:.2f} s: {} lookups ({} buffer, {} image, {} clean), {} stalls "
             "({:.2f} ms waited), {} regions; prefetch {} writes {} KiB; preflush marks {}",
             seconds, state.lookups, state.buffer_lookups, state.image_lookups,
             state.lookups - state.buffer_lookups - state.image_lookups, state.stalls,
             static_cast<double>(state.stall_ns) / 1e6, state.regions.size(),
             state.prefetch_writes, state.prefetch_bytes / 1024, state.preflush_marks);
    std::vector<std::pair<u64, Region>> sorted(state.regions.begin(), state.regions.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        if (a.second.stalls != b.second.stalls) {
            return a.second.stalls > b.second.stalls;
        }
        return a.second.lookups > b.second.lookups;
    });
    for (size_t i = 0; i < (std::min<size_t>)(sorted.size(), 16); ++i) {
        const auto& [start, region] = sorted[i];
        LOG_INFO(HW_GPU, "  [{:#x}, {:#x}) +{:#x} {}: {} stalls ({:.2f} ms), {} lookups", start,
                 region.end, region.end - start, KindName(region.kind), region.stalls,
                 static_cast<double>(region.stall_ns) / 1e6, region.lookups);
    }
    state.regions.clear();
    state.lookups = 0;
    state.buffer_lookups = 0;
    state.image_lookups = 0;
    state.stalls = 0;
    state.stall_ns = 0;
    state.prefetch_writes = 0;
    state.prefetch_bytes = 0;
    state.preflush_marks = 0;
}

/// The caches answered a read of [start, end) with an area of `kind` (whether it stalls is
/// counted by NoteStall).
inline void NoteLookup(u64 start, u64 end, Kind kind) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.lookups;
    if (kind == Kind::Buffer) {
        ++state.buffer_lookups;
    } else if (kind == Kind::Image) {
        ++state.image_lookups;
    }
    Region& region = state.regions[start];
    region.end = (std::max)(region.end, end);
    if (kind != Kind::Clean) {
        region.kind = kind;
    }
    ++region.lookups;
    MaybeReportLocked(state);
}

/// A read of [start, end) waited `ns` for the GPU thread and the host GPU.
inline void NoteStall(u64 start, u64 end, u64 ns) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.stalls;
    state.stall_ns += ns;
    Region& region = state.regions[start];
    region.end = (std::max)(region.end, end);
    ++region.stalls;
    region.stall_ns += ns;
    MaybeReportLocked(state);
}

/// A GPU write queued `bytes` of CPU-read pages for a copy back at the next fence.
inline void NotePrefetch(u64 bytes) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.prefetch_writes;
    state.prefetch_bytes += bytes;
}

/// A read marked a page range Preflushable (EDEN_NX2_REACTIVE_PREFLUSH).
inline void NotePreflushMark() {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.preflush_marks;
}

} // namespace VideoCommon::Nx2Reactive
