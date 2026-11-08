// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "common/logging.h"
#include "common/cpu_features.h"
#include "common/thread_cpu_clock.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    WriterWait,
    ListSubmitWait,
    ListGpuWait,
    TickWork,       ///< GPU::TickWork (sync requests: reactive flushes)
    InlineToMemory, ///< AccelerateInlineToMemory
    // The split back's own work, inside what was "other" and "method" (count: entries or calls).
    RingDecode,   ///< BackLoop's batches: entry decode and dispatch, Engine entries, method sinks
    DeltaApply,   ///< Kind::StateDelta (Maxwell3D::ApplyFrontDelta)
    CbWrite,      ///< Kind::CbWrite (Maxwell3D::BackCbWrite)
    CbBind,       ///< Kind::CbBind and bind group methods (Maxwell3D::ProcessCBBind)
    CbMulti,      ///< Constant buffer data in method runs (Maxwell3D::ProcessCBMultiData)
    DrawBatch,    ///< Kind::DrawBatch expansion (Maxwell3D::RunDrawBatch)
    DrawRun,      ///< Kind::DrawRun (Maxwell3D::RunDrawRun), the draws in it included
    DrawManager,  ///< DrawManager begin/end/deferred, outside the rasterizer's draw
    Method3D,     ///< Maxwell3D method entries ("method" keeps the other engines and runs)
    Call,         ///< Kind::Call entries, by thunk in the "calls" report line
    FinishMacro,  ///< Maxwell3D::FinishFrontMacro (a Call after each front macro that drew)
    QueryReport,  ///< report_semaphore.query (Maxwell3D::ProcessQueryGet)
    CounterReset, ///< clear_report_value (Maxwell3D::ProcessCounterReset)
    RenderEnable, ///< render_enable.mode (Maxwell3D::ProcessQueryCondition)
    SyncPoint,    ///< sync_info (Maxwell3D::ProcessSyncPoint)
    WaitForIdle,  ///< wait_for_idle (rasterizer WaitForIdle)
    CacheBarrier, ///< invalidate_texture_data_cache, fragment and tiled cache barriers
    IdleHook,     ///< The back's idle hook (Scheduler::FlushDeferred: submits)
    // The split back's method entries by engine (MethodScope); Method3D above is Maxwell3D's.
    MethodCompute, ///< KeplerCompute method entries
    MethodDma,     ///< MaxwellDMA method entries (copies, the texture uploads below them excluded)
    Method2D,      ///< Fermi2D method entries
    MethodInline,  ///< KeplerMemory method entries (inline uploads)
    ComputeWrites, ///< Kind::Writes entries (KeplerCompute::BackWrites)
    // Inside "update buffers" and "bind stage buffers": graphics uniform and storage buffers.
    UpdateStorage, ///< BufferCache::UpdateStorageBuffers (storage pointer resolve), per stage
    BindUniforms,  ///< BufferCache::BindHostGraphicsUniformBuffers, per stage
    BindStorage,   ///< BufferCache::BindHostGraphicsStorageBuffers, per stage with storage
    StoragePointer, ///< BufferCache::BindGraphicsStorageBuffer (pointer read), per graphics binding
    // Inside the costliest non-draw handlers of the split back (OoT field profile).
    ClearFlush,         ///< RasterizerVulkan::Clear: FlushWork and FlushCaching
    ClearTargets,       ///< RasterizerVulkan::Clear: UpdateRenderTargets and GetFramebuffer
    RenderTargetLookup, ///< TextureCache::RescaleRenderTargets (render target views, draws too)
    RenderEnableFlush,  ///< Conditional rendering: FlushCaching before the query lookup
    RenderEnableRead,   ///< Conditional rendering decided on the CPU: the predicate's read
    QmdSignal,          ///< KeplerCompute: the QMD scheduler (Signal), dispatches excluded
    QmdRelease,         ///< KeplerCompute: a QMD's semaphore releases (fences, payload queries)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait",
    "writer wait", "list submit wait", "list GPU wait", "tick work", "inline to memory",
    "ring decode", "delta apply", "cb write", "cb bind", "cb multi", "draw batch", "draw run",
    "draw manager", "method 3D", "call", "finish macro", "query report", "counter reset",
    "render enable", "syncpoint", "wait for idle", "cache barrier", "idle hook",
    "method compute", "method DMA", "method 2D", "method inline", "compute writes",
    "update storage", "bind uniforms", "bind storage", "storage pointer",
    "clear flush", "clear targets", "render target lookup", "render enable flush",
    "render enable read", "QMD signal", "QMD release",
};
static_assert(zone_names[static_cast<size_t>(Zone::Count) - 1] != nullptr,
              "every zone needs a name");

// A plain flag: Enabled() runs several times per method write (no guard to check).
inline bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;
    VideoCommon::GpuSplit::profile_calls = g_enabled;
}

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

// What the zones and drain waits measure in: the time stamp counter without the fences the wall
// clock reads it with (those fences cost ~10% of the GPU back thread with ~45,000 draws a
// second). Report converts it with the wall clock.
inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return Now();
#endif
}

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,           ///< Buffer data copied back from the host GPU at fences
    DownloadCopies,          ///< Buffer ranges copied back
    DmaReadBytes,            ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,            ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites,         ///< Of MethodWrites: the register already held the value
    AcquireChecks,           ///< Semaphore acquires evaluated
    AcquireWaits,            ///< Of AcquireChecks: not met yet (the channel waits)
    DeltaWords,              ///< Split back: StateDelta words applied
    DeltaSameWords,          ///< Of DeltaWords: the register held the value already
    CbWriteWords,            ///< Split back: words in Kind::CbWrite entries
    BatchRepeats,            ///< Split back: instances a DrawBatch counted without replaying them
    RingWords,               ///< Split back: ring words consumed
    RingEntries,             ///< Split back: ring entries executed
    FusedWrites,   ///< Split back: compute writes in Kind::Writes entries (EDEN_NX2_COMPUTE_FUSE)
    MergedUploads, ///< Split back: inline upload flushes joined to the one before (I2M_MERGE)
    RunDraws,      ///< Split back: draw.end words in Kind::DrawRun entries
    DispatchNoBarrier,      ///< Guest dispatches recorded without the barrier before them
    DispatchBarrierOther,   ///< ... with it: other work was recorded since the last dispatch
    DispatchBarrierUnknown, ///< ... with it: accesses not fully known (indirect, replaced, ...)
    DispatchBarrierRaw,     ///< ... with it: reads what a dispatch since the barrier wrote
    DispatchBarrierWaw,     ///< ... with it: writes what a dispatch since the barrier wrote
    DispatchBarrierWar,     ///< ... with it: writes what a dispatch since the barrier read
    // The split front's QMD view (FrontQmdView): signals whose releases it noted when sent,
    // signals that waited for the back and of them those valued late, signals sent while it had
    // no table, and the times it lost its table.
    QmdValued,
    QmdWaited,
    QmdLate,
    QmdUnsynced,
    QmdLost,
    Count,
};

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, static_cast<size_t>(Counter::Count)> counters{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_ns{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_count{};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
    u64 window_start_ticks{};
    std::optional<u64> cpu_start_ns;
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Ticks();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Ticks() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ns{};
};

/// Time per method entry on the split back, inclusive of the zones below it, by engine and
/// method (MethodScope). Allocated on the first timed entry: only the back thread has one.
struct MethodTimes {
    static constexpr u32 Engines = 6; ///< Tegra::Engines::EngineTypes
    static constexpr u32 Methods = 0x2000;
    std::array<u64, Engines * Methods> ticks{};
    std::array<u32, Engines * Methods> count{};
};
inline thread_local std::unique_ptr<MethodTimes> t_method_times;

/// Exclusive zone of an engine's method entries (Tegra::Engines::EngineTypes order).
inline Zone EngineZone(u32 engine) {
    switch (engine) {
    case 1:
        return Zone::MethodCompute;
    case 2:
        return Zone::Method3D;
    case 3:
        return Zone::Method2D;
    case 4:
        return Zone::MethodDma;
    case 5:
        return Zone::MethodInline;
    default:
        return Zone::Method;
    }
}

/// A Scope for one method entry of the split back: its engine's zone (exclusive), and its time
/// with what it runs (draws, uploads) under its method in MethodTimes. One flag check when off.
class MethodScope {
public:
    MethodScope(u32 engine_, u32 method_) : enabled{Enabled()} {
        if (!enabled) [[likely]] {
            return;
        }
        engine = engine_;
        method = method_;
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Ticks();
    }

    ~MethodScope() {
        if (!enabled) [[likely]] {
            return;
        }
        const u64 elapsed = Ticks() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(EngineZone(engine));
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
        if (!t_method_times) {
            t_method_times = std::make_unique<MethodTimes>();
        }
        const size_t key = size_t{engine % MethodTimes::Engines} * MethodTimes::Methods +
                           (method % MethodTimes::Methods);
        t_method_times->ticks[key] += elapsed;
        ++t_method_times->count[key];
    }

    MethodScope(const MethodScope&) = delete;
    MethodScope& operator=(const MethodScope&) = delete;

private:
    bool enabled;
    u32 engine{};
    u32 method{};
    u64 start{};
    u64 parent_child_ns{};
};

/// The method entries that took the most time, per second: engine, method, time (inclusive)
/// and entries.
inline std::string TopMethodTimes(const MethodTimes& times, u64 window, u64 window_ticks) {
    static constexpr std::array<const char*, MethodTimes::Engines> engine_names{
        "timer", "compute", "3D", "2D", "DMA", "inline"};
    std::vector<u32> order;
    for (u32 key = 0; key < times.ticks.size(); ++key) {
        if (times.count[key] != 0) {
            order.push_back(key);
        }
    }
    const size_t top = (std::min<size_t>)(order.size(), 24);
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [&times](u32 a, u32 b) { return times.ticks[a] > times.ticks[b]; });
    std::string text;
    for (size_t i = 0; i < top; ++i) {
        const u32 key = order[i];
        text += fmt::format(
            " {} {:#x} {:.2f} ms ({:.0f}/s),", engine_names[key / MethodTimes::Methods],
            key % MethodTimes::Methods,
            static_cast<double>(times.ticks[key]) * 1000.0 / static_cast<double>(window_ticks),
            static_cast<double>(times.count[key]) * 1e9 / static_cast<double>(window));
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Kind::Call entries of the split back by thunk: where the front deferred them (GpuSplit::Defer
/// registers the site while profiling) and their time, inclusive of the zones below them.
struct CallSites {
    std::mutex mutex;
    std::unordered_map<u64, std::string> names;
};
inline CallSites& GetCallSites() {
    static CallSites sites;
    return sites;
}
inline void RegisterCallSite(u64 thunk, const char* file, u32 line) {
    auto& sites = GetCallSites();
    std::scoped_lock lock{sites.mutex};
    if (!sites.names.contains(thunk)) {
        std::string_view name{file};
        if (const auto slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
            name.remove_prefix(slash + 1);
        }
        sites.names.emplace(thunk, fmt::format("{}:{}", name, line));
    }
}
struct CallTime {
    u64 ticks{};
    u64 count{};
};
inline thread_local std::unordered_map<u64, CallTime> t_call_times;

/// Times one Kind::Call entry: the "call" zone (exclusive) and its thunk's inclusive time.
class CallScope {
public:
    explicit CallScope(u64 thunk_) : enabled{Enabled()} {
        if (!enabled) [[likely]] {
            return;
        }
        thunk = thunk_;
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Ticks();
    }

    ~CallScope() {
        if (!enabled) [[likely]] {
            return;
        }
        const u64 elapsed = Ticks() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(Zone::Call);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
        CallTime& time = t_call_times[thunk];
        time.ticks += elapsed;
        ++time.count;
    }

    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;

private:
    bool enabled;
    u64 thunk{};
    u64 start{};
    u64 parent_child_ns{};
};

inline std::string TopCallTimes(u64 window, u64 window_ticks) {
    std::vector<std::pair<u64, CallTime>> order(t_call_times.begin(), t_call_times.end());
    const size_t top = (std::min<size_t>)(order.size(), 16);
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
    auto& sites = GetCallSites();
    std::scoped_lock lock{sites.mutex};
    std::string text;
    for (size_t i = 0; i < top; ++i) {
        const auto it = sites.names.find(order[i].first);
        text += fmt::format(
            " {} {:.2f} ms ({:.0f}/s),",
            it != sites.names.end() ? it->second : fmt::format("{:#x}", order[i].first),
            static_cast<double>(order[i].second.ticks) * 1000.0 / static_cast<double>(window_ticks),
            static_cast<double>(order[i].second.count) * 1e9 / static_cast<double>(window));
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Method calls on this thread by method (3D registers on the back, macros on the front), and
/// same-value writes among them; MME DMA reads by the macro that made them. The most frequent
/// are logged with the zones.
inline thread_local std::array<u32, 0x2000> t_method_count{};
inline thread_local std::array<u32, 0x2000> t_method_same{};
inline thread_local std::array<u32, 0x2000> t_dma_reads{};
/// Split front: macro calls without a native replacement (macro_hle_oot.h), which the JIT ran.
inline thread_local std::array<u32, 0x2000> t_jit_macros{};

inline void CountMethod(u32 method, bool same) {
    if (Enabled() && method < t_method_count.size()) {
        ++t_method_count[method];
        t_method_same[method] += same ? 1 : 0;
    }
}

inline void CountJitMacro(u32 macro) {
    if (Enabled() && macro < t_jit_macros.size()) {
        ++t_jit_macros[macro];
    }
}

inline void CountDmaRead(u32 macro) {
    if (Enabled() && macro < t_dma_reads.size()) {
        ++t_dma_reads[macro];
    }
}

inline std::string TopMethods(const std::array<u32, 0x2000>& counts,
                              const std::array<u32, 0x2000>* same, u64 window) {
    const double window_d = static_cast<double>(window);
    std::array<u32, 0x2000> order;
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    constexpr size_t top = 24;
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [&counts](u32 a, u32 b) { return counts[a] > counts[b]; });
    std::string text;
    for (size_t i = 0; i < top && counts[order[i]] != 0; ++i) {
        const u32 method = order[i];
        text += fmt::format(" {:#x} {:.0f}/s", method,
                            static_cast<double>(counts[method]) * 1e9 / window_d);
        if (same != nullptr && (*same)[method] != 0) {
            text += fmt::format(" ({:.0f} same)",
                                static_cast<double>((*same)[method]) * 1e9 / window_d);
        }
        text += ',';
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// The front waited for the back since `start` (Ticks) for `site`.
inline void AddDrain(DrainSite site, u64 start) {
    if (Enabled()) {
        const auto index = static_cast<size_t>(site);
        t_data.drain_ns[index] += Ticks() - start;
        ++t_data.drain_count[index];
    }
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s. Returns whether it logged (the window just ended).
inline bool Report(const char* thread_name = "GPU thread") {
    if (!Enabled()) {
        return false;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.window_start_ticks = Ticks();
        data.cpu_start_ns = Common::CurrentThreadCpuTimeNs();
        data.child_ns = 0;
        return false;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return false;
    }
    const auto cpu_now = Common::CurrentThreadCpuTimeNs();
    const u64 now_ticks = Ticks();
    const u64 window_ticks = (std::max<u64>)(now_ticks - data.window_start_ticks, 1);
    const double window_d = static_cast<double>(window);
    const double ticks_d = static_cast<double>(window_ticks);
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window_ticks > zoned ? window_ticks - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / ticks_d;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window_d;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window_d,
                        static_cast<double>(data.counters[1]) * 1e9 / window_d,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window_d,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window_d);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window_d,
                        static_cast<double>(data.counters[5]) * 1e9 / window_d);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / window_d,
                        static_cast<double>(data.counters[7]) * 1e9 / window_d);
    if (data.counters[static_cast<size_t>(Counter::RingEntries)] != 0) {
        const auto per_second = [&](Counter counter) {
            return static_cast<double>(data.counters[static_cast<size_t>(counter)]) * 1e9 /
                   window_d;
        };
        line += fmt::format(" | ring {:.0f} entries {:.0f} words | delta words {:.0f} ({:.0f} "
                            "same on the back) | cb write words {:.0f} | batch repeats {:.0f} | "
                            "fused compute writes {:.0f} | merged uploads {:.0f} | run draws "
                            "{:.0f}",
                            per_second(Counter::RingEntries), per_second(Counter::RingWords),
                            per_second(Counter::DeltaWords), per_second(Counter::DeltaSameWords),
                            per_second(Counter::CbWriteWords), per_second(Counter::BatchRepeats),
                            per_second(Counter::FusedWrites), per_second(Counter::MergedUploads),
                            per_second(Counter::RunDraws));
    }
    const auto per_counter = [&](Counter counter) {
        return data.counters[static_cast<size_t>(counter)];
    };
    if (const u64 dispatches = per_counter(Counter::DispatchNoBarrier) +
                               per_counter(Counter::DispatchBarrierOther) +
                               per_counter(Counter::DispatchBarrierUnknown) +
                               per_counter(Counter::DispatchBarrierRaw) +
                               per_counter(Counter::DispatchBarrierWaw) +
                               per_counter(Counter::DispatchBarrierWar);
        dispatches != 0) {
        const auto rate = [&](Counter counter) {
            return static_cast<double>(per_counter(counter)) * 1e9 / window_d;
        };
        line += fmt::format(" | dispatch barriers: none {:.0f}, other work {:.0f}, unknown {:.0f}, "
                            "RAW {:.0f}, WAW {:.0f}, WAR {:.0f}",
                            rate(Counter::DispatchNoBarrier), rate(Counter::DispatchBarrierOther),
                            rate(Counter::DispatchBarrierUnknown), rate(Counter::DispatchBarrierRaw),
                            rate(Counter::DispatchBarrierWaw), rate(Counter::DispatchBarrierWar));
    }
    if (const u64 signals = data.counters[static_cast<size_t>(Counter::QmdValued)] +
                            data.counters[static_cast<size_t>(Counter::QmdWaited)] +
                            data.counters[static_cast<size_t>(Counter::QmdUnsynced)];
        signals != 0) {
        const auto per_second = [&](Counter counter) {
            return static_cast<double>(data.counters[static_cast<size_t>(counter)]) * 1e9 /
                   static_cast<double>(window);
        };
        line += fmt::format(" | QMD view: {:.0f} signals valued, {:.0f} waited ({:.0f} valued "
                            "late), {:.0f} without a table, {:.0f} tables lost",
                            per_second(Counter::QmdValued), per_second(Counter::QmdWaited),
                            per_second(Counter::QmdLate), per_second(Counter::QmdUnsynced),
                            per_second(Counter::QmdLost));
    }
    std::string drains;
    for (size_t i = 0; i < data.drain_count.size(); ++i) {
        if (data.drain_count[i] != 0) {
            drains += fmt::format(" {} {:.1f} ms ({:.0f}/s),", drain_site_names[i],
                                  static_cast<double>(data.drain_ns[i]) * 1000.0 / ticks_d,
                                  static_cast<double>(data.drain_count[i]) * 1e9 / window_d);
        }
    }
    if (!drains.empty()) {
        drains.pop_back();
        line += " | waits for the back by site:" + drains;
    }
    {
        // What the zones cost themselves: two time stamps each, most of it charged to the zone
        // around them ("ring decode" on the back). Two reads of the counter measured once.
        static const u64 stamp_pair_ticks = [] {
            constexpr u32 Rounds = 4096;
            const u64 begin = Ticks();
            u64 sink = 0;
            for (u32 i = 0; i < Rounds; ++i) {
                sink += Ticks();
            }
            const u64 end = Ticks();
            return (end - begin + (sink & 1)) * 2 / Rounds;
        }();
        u64 scopes = 0;
        for (const u64 count : data.count) {
            scopes += count;
        }
        line += fmt::format(" | zone scopes {:.0f}/s, their time stamps ~{:.1f} ms",
                            static_cast<double>(scopes) * 1e9 / static_cast<double>(window),
                            static_cast<double>(scopes * stamp_pair_ticks) * 1000.0 /
                                static_cast<double>(window_ticks));
    }
    if (cpu_now && data.cpu_start_ns && *cpu_now >= *data.cpu_start_ns) {
        line += fmt::format(" | host_cpu_busy_ms_per_s={:.3f} window_wall_ms={:.3f}",
                            static_cast<double>(*cpu_now - *data.cpu_start_ns) * 1000.0 / window_d,
                            static_cast<double>(window) / 1e6);
    } else {
        line += fmt::format(" | host_cpu_busy_ms_per_s=unavailable window_wall_ms={:.3f}",
                            static_cast<double>(window) / 1e6);
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    if (const std::string methods = TopMethods(t_method_count, &t_method_same, window); !methods.empty()) {
        LOG_INFO(HW_GPU, "{} methods per second:{}", thread_name, methods);
    }
    if (const std::string reads = TopMethods(t_dma_reads, nullptr, window); !reads.empty()) {
        LOG_INFO(HW_GPU, "{} MME DMA reads per second by macro:{}", thread_name, reads);
    }
    if (const std::string jit = TopMethods(t_jit_macros, nullptr, window); !jit.empty()) {
        LOG_INFO(HW_GPU, "{} macros run by the JIT per second:{}", thread_name, jit);
    }
    if (t_method_times) {
        if (const std::string times = TopMethodTimes(*t_method_times, window, window_ticks);
            !times.empty()) {
            LOG_INFO(HW_GPU, "{} method entries by time per second (with what they ran):{}",
                     thread_name, times);
        }
        t_method_times->ticks.fill(0); // not `= {}`: a 576 KiB temporary on the stack
        t_method_times->count.fill(0);
    }
    if (!t_call_times.empty()) {
        LOG_INFO(HW_GPU, "{} calls by time per second (deferred at):{}", thread_name,
                 TopCallTimes(window, window_ticks));
        t_call_times.clear();
    }
    t_method_count = {};
    t_method_same = {};
    t_dma_reads = {};
    t_jit_macros = {};
    data = {};
    data.window_start = now;
    data.window_start_ticks = now_ticks;
    data.cpu_start_ns = cpu_now;
    return true;
}

} // namespace VideoCommon::Nx2Profile
