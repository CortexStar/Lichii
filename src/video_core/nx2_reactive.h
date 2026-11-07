// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
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

/// EDEN_NX2_REACTIVE_PREFETCH (on unless 0): GPU writes over pages the guest CPU has read GPU
/// data from (BufferCache::cpu_read_ranges) are also copied back at the next fence; their traps
/// stay armed. A read after the copy reaches guest memory goes on without a stall and sees
/// current data; an earlier one waits for the copy (WaitLanding) or flushes. A pop skips the
/// pages with guest CPU writes not yet collected (SetPendingCpuWritesQuery), so it cannot write
/// the GPU's older bytes over them.
inline bool Prefetch() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_REACTIVE_PREFETCH");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_REACTIVE_PREFLUSH=1: upstream yuzu's reactive flushing on Switch 2 titles too: the
/// first read of a GPU-written page flushes it and marks it Preflushable, so later GPU writes over
/// it are copied back at fences and its reads never trap again. Off by default: reads that come
/// before the copy lands see older data with nothing to tell, which flickered Ocarina's tone
/// mapping and left a third of Castle Town loads overexposed in other forks (Frizti fbc7b11276,
/// Lichii #259, Eglantine #686).
inline bool Preflush() {
    static const bool enabled = EnvFlag("EDEN_NX2_REACTIVE_PREFLUSH");
    return enabled;
}

/// EDEN_NX2_REACTIVE_WAIT_LANDING (on unless 0): a guest read of a GPU-written page whose GPU
/// writes all have a download recorded (Prefetch) waits for those downloads to land instead of
/// flushing: the GPU thread neither copies again nor waits for the host GPU (Scheduler::Finish).
/// After 16 ms without the copy landing the read flushes as before.
inline bool WaitLanding() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_REACTIVE_WAIT_LANDING");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_REACTIVE_ASYNC_FLUSH (on unless 0): a guest read of GPU-written buffer data with no
/// download on its way has the GPU thread queue that data for download at a fence it submits at
/// once (RasterizerInterface::QueueFlushRegion), then waits for the copy to land as WaitLanding
/// does, instead of the GPU thread copying it and waiting for the host GPU to go idle
/// (Scheduler::Finish). The copy holds what the GPU wrote up to the GPU thread's position when it
/// took the request, as the flush's did, and the read goes on only once guest memory has it (no
/// stale data, unlike preflushing). Needs Prefetch and WaitLanding; image data still flushes.
inline bool AsyncFlush() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_REACTIVE_ASYNC_FLUSH");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_REACTIVE_HOT_FRAMES (default 64, 0 never forgets): pages the guest CPU read GPU data
/// from keep their later GPU writes copied back (Prefetch) while they are read at least once every
/// 64 to 128 frames. A forgotten page's next read flushes as a first read does.
inline u64 HotFrames() {
    static const u64 frames = [] {
        const char* value = std::getenv("EDEN_NX2_REACTIVE_HOT_FRAMES");
        return value != nullptr ? static_cast<u64>(std::strtoull(value, nullptr, 10)) : u64{64};
    }();
    return frames;
}

/// Frames ended so far (FenceManager::TickFrame).
inline std::atomic<u64> g_frame{};

/// Wakes the reads waiting for downloads to land (WaitLanding) when a fence's downloads pop.
struct Landing {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<u64> generation{};
    std::atomic<u32> waiters{};
};

inline Landing& GetLanding() {
    static Landing landing;
    return landing;
}

inline void NotifyLanded() {
    Landing& landing = GetLanding();
    landing.generation.fetch_add(1, std::memory_order_acq_rel);
    if (landing.waiters.load(std::memory_order_acquire) != 0) {
        std::scoped_lock lock{landing.mutex};
        landing.cv.notify_all();
    }
}

/// Whether guest CPU writes to [address, address + size) were collected but not yet applied to
/// the caches (GPU::InvalidateGPUCache). Set by the GPU; a fence's downloads must not land over
/// them.
inline std::function<bool(u64, u64)>& PendingCpuWritesQuery() {
    static std::function<bool(u64, u64)> query;
    return query;
}

inline void SetPendingCpuWritesQuery(std::function<bool(u64, u64)> query) {
    PendingCpuWritesQuery() = std::move(query);
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
    u64 landing_waits{};
    u64 landing_timeouts{};
    u64 landing_ns{};
    u64 async_queued{};
    u64 async_fallbacks{};
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
             "({:.2f} ms waited), {} regions; prefetch {} writes {} KiB; preflush marks {}; "
             "landing waits {} ({} timed out, {:.2f} ms); async flushes {} ({} flushed after)",
             seconds, state.lookups, state.buffer_lookups, state.image_lookups,
             state.lookups - state.buffer_lookups - state.image_lookups, state.stalls,
             static_cast<double>(state.stall_ns) / 1e6, state.regions.size(),
             state.prefetch_writes, state.prefetch_bytes / 1024, state.preflush_marks,
             state.landing_waits, state.landing_timeouts,
             static_cast<double>(state.landing_ns) / 1e6, state.async_queued,
             state.async_fallbacks);
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
    state.landing_waits = 0;
    state.landing_timeouts = 0;
    state.landing_ns = 0;
    state.async_queued = 0;
    state.async_fallbacks = 0;
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

/// A read waited `ns` for downloads to land (WaitLanding); `landed` false when it gave up.
inline void NoteLandingWait(bool landed, u64 ns) {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.landing_waits;
    state.landing_timeouts += landed ? 0 : 1;
    state.landing_ns += ns;
    MaybeReportLocked(state);
}

/// A read's data was queued for download (AsyncFlush); `landed` false when it flushed after all.
inline void NoteAsyncFlush(bool queued, bool landed) {
    if (!queued) {
        return;
    }
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.async_queued;
    state.async_fallbacks += landed ? 0 : 1;
}

/// A read marked a page range Preflushable (EDEN_NX2_REACTIVE_PREFLUSH).
inline void NotePreflushMark() {
    auto& state = Get();
    std::scoped_lock lock{state.mutex};
    ++state.preflush_marks;
}

} // namespace VideoCommon::Nx2Reactive
