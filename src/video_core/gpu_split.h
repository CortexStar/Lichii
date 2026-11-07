// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

/// Why the split's front waited for its back (EDEN_NX2_GPU_PROFILE counts each).
enum class DrainSite : u32 {
    Other,
    FetchDrain,     ///< EDEN_NX2_FETCH_DRAIN: before every entry
    Acquire,        ///< A semaphore acquire reads the value the back may release
    SemaphoreFlush, ///< The acquired semaphore is in the buffer cache
    FenceRelease,   ///< The puller's acquires release the pending fences
    ListWriters,    ///< GPU-written command lists: the dispatches and copies sent before them
    ListSync,       ///< Command lists on draw-written pages: the work before the last host sync
    ListFlush,      ///< Command lists on GPU-written pages: drain and flush the pages
    MacroDmaRead,   ///< The front's macros read memory (MME DMA)
    MacroDmaFlush,  ///< Those words are in the buffer cache
    GpSyncFence,    ///< A GP entry's sync bit (sync_memory_operations)
    Map,            ///< GPU memory maps before later command reads
    FrontStart,     ///< The front takes an engine's macros over
    FrontVerify,    ///< EDEN_NX2_FRONT_VERIFY
    NarrowMacroDmaRead, ///< MacroDmaRead narrowed (WaitForWritesTo)
    NarrowAcquire,      ///< Acquire narrowed: an acquire already met
    NarrowListWriters,  ///< ListWriters narrowed (WaitForWritersTo)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(DrainSite::Count)> drain_site_names{
    "other",       "fetch drain",     "acquire",    "semaphore flush", "fence release",
    "list writers", "list sync",      "list flush", "macro dma read",  "macro dma flush",
    "gp sync fence", "map",           "front start", "front verify",   "macro dma read (narrow)",
    "acquire (narrow)", "list writers (narrow)",
};

/// What made the front note an unbounded write (EDEN_NX2_NARROW_DIAG attributes waits to it).
enum class UnboundedCause : u8 {
    Unknown,
    RenderEnable,     ///< 3D render_enable.mode (conditional rendering)
    Wfi3D,            ///< 3D wait_for_idle
    TexInvalidate,    ///< 3D invalidate_texture_data_cache
    Inline3D,         ///< 3D inline upload without a known destination
    Maxwell3DBack,    ///< 3D methods with the macros on the back
    ComputeMacro,     ///< A compute macro call (per parameter word)
    ComputeLaunch,    ///< A compute dispatch (QMD launch, inline QMD)
    ComputeUpload,    ///< Compute inline upload (exec/data upload)
    ComputeReport,    ///< Compute report semaphore
    ComputeOther,     ///< Other compute methods MayWriteMemory counts
    BlockLinearDma,   ///< Copy engine launch with writes the front can't bound
    OtherEngine,      ///< Fermi2D blits, KeplerMemory uploads
    PullerWfi,        ///< Puller WaitForIdle
    SyncpointAcquire, ///< Puller fence action acquire (forced fence release)
    MemOpB,
    GpSync,           ///< A GP entry's sync bit
    GpuThreadCommand, ///< GPU thread commands (maps, flushes, syncs)
    Overflow,         ///< The pending write records overflowed
    Record,           ///< (Wait attribution only) a recorded write overlapping the read
    Sync,             ///< (Wait attribution only) the last host-level wait
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(UnboundedCause::Count)>
    unbounded_cause_names{
        "unknown",        "render_enable",     "3D WFI",          "tex invalidate",
        "3D inline",      "3D (back macros)",  "compute macro",   "compute launch",
        "compute upload", "compute report",    "compute other",   "block-linear DMA",
        "fermi2d/inline", "puller WFI",        "syncpoint acquire", "MemOpB",
        "GP sync",        "GPU thread cmd",    "overflow",        "record",
        "sync",
};

/// Which unbounded position a narrowed front read waits for (GpuSplit::WaitForWritesTo).
enum class NarrowBase : u8 {
    Read,     ///< Every unbounded write (macro DMA reads)
    Acquire,  ///< Every unbounded write and dispatch (semaphore acquires)
    Recorded, ///< Only the recorded writes (and overflowed records): EDEN_NX2_NARROW_DMA_RECORDED
};

/// An EDEN_NX2_* on/off switch: '0' off, anything else on, unset `default_on`.
[[nodiscard]] inline bool Nx2Switch(const char* name, bool default_on) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == 0) {
        return default_on;
    }
    return value[0] != '0';
}

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Whether the calling thread is the back.
    [[nodiscard]] static bool OnBackThread() {
        return on_back_thread;
    }
    /// Called on the back before it sleeps waiting for work (the rasterizer submits deferred work).
    void SetIdleHook(void (*hook)(void*), void* argument) {
        idle_hook = hook;
        idle_argument = argument;
    }
    /// Any thread: whether the back thread runs (false before it starts and once it stopped).
    [[nodiscard]] bool BackRunning() const {
        return back_running.load(std::memory_order_acquire);
    }
    /// Any thread: has the back call the idle hook soon, between entries or at once if it sleeps.
    void RequestIdleHook() {
        hook_requested.store(true, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
    }

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    /// Work that writes guest memory notes it after (NoteWrite, NoteUnboundedWrite).
    template <typename Func>
    void Defer(const Func& func) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func);
    /// Runs `func` on the back and waits for it: for work on back-owned state, such as a buffer
    /// cache download that records into the Vulkan scheduler. A drained back still submits from
    /// its idle hook, so the front never touches that state itself.
    template <typename Func>
    void Run(const Func& func, DrainSite site = DrainSite::Other) {
        if (on_back_thread || !active) {
            func();
            return;
        }
        Defer(func);
        Drain(site);
    }

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far.
    void Drain(DrainSite site = DrainSite::Other);

    /// Compute or copy work was just sent: what can write GPU-written command lists.
    void NoteWriter() {
        last_writer_position = write_position;
    }
    /// Waits until the back executed the compute and copy work sent so far (not what came after).
    /// Returns whether any of it was still queued.
    bool WaitForWriters();
    /// A host-level wait was just sent (a syncpoint acquire, a host WFI, a GP entry with its sync
    /// bit; semaphore acquires drain): the guest's GPU work before it is complete before the
    /// commands after it are fetched.
    void NoteSync() {
        last_sync_position = write_position;
    }
    /// Waits until the back executed the work sent before the last NoteSync. Returns whether any
    /// of it was still queued.
    bool WaitForSync();

    // Narrow drains (EDEN_NX2_NARROW_DRAIN, on unless 0; only with the macros on the front, whose
    // register copies give the destinations): a front read of guest memory waits only for the
    // queued work that may write what it reads, not for everything queued. The front records
    // each queued write whose destination it knows (NoteWrite: 3D constant buffer data, inline
    // uploads and reports, semaphore releases, copy engine launches) and the position of the
    // latest work that may write memory it can't bound (NoteUnboundedWrite: dispatches, blits,
    // copies into images, forced fence releases, host waits, GPU thread commands). Draws only
    // write storage buffers (and transform feedback), whose ranges the front can't know either;
    // the guest orders them before a read from the command stream with a host wait (WFI, GP
    // entry sync, syncpoint acquire), which counts as an unbounded write. EDEN_NX2_NARROW_DRAIN=2
    // counts every 3D method as one too (as exact as the full drain, without the draw rule).
    // Readers still drain fully for memory GPU work wrote through the buffer cache (newer words
    // may be in its caches): MemoryManager::Nx2ReadNeedsDrain.
    [[nodiscard]] bool NarrowDrains() const {
        return narrow;
    }
    /// EDEN_NX2_NARROW_VERIFY=1 (debug): narrowed reads are compared with a full drain.
    [[nodiscard]] static bool NarrowVerify();
    /// EDEN_NX2_NARROW_VERIFY: logs, now and then, narrowed reads that differed from a full drain
    /// (`mismatches`: words that differed; `macro`: the macro that read, 0 if none).
    static void ReportNarrowCheck(DrainSite site, u32 mismatches, u32 macro, GPUVAddr address);
    /// Counts starts of the split: engines keeping front copies of their registers rebuild them.
    [[nodiscard]] u64 Session() const {
        return session;
    }
    /// The work just sent writes [address, address + size) of the GPU address space `space` (its
    /// MemoryManager) when the back runs it.
    void NoteWrite(const void* space, GPUVAddr address, u64 size) {
        if (narrow && size != 0) {
            RecordWrite(space, address, size);
        }
    }
    /// The work just sent may write guest memory the front can't bound. `writer`: it is compute
    /// or copy work (what WaitForWriters waits for).
    void NoteUnboundedWrite(bool writer = false, UnboundedCause cause = UnboundedCause::Unknown) {
        if (narrow) {
            unbounded_position = write_position;
            unbounded_cause = cause;
            acquire_position = write_position;
            acquire_cause = cause;
            if (writer) {
                unbounded_writer_position = write_position;
                writer_cause = cause;
            }
            if (diag) [[unlikely]] {
                ++diag_notes[static_cast<size_t>(cause)];
            }
        }
    }
    /// A compute dispatch was just sent. EDEN_NX2_NARROW_COMPUTE: the front end runs MME DMA
    /// reads in method order without waiting for earlier grids (barriers come as WFIs, which
    /// count as unbounded), so dispatches only hold semaphore acquires and command list reads.
    void NoteDispatch(UnboundedCause cause = UnboundedCause::ComputeLaunch) {
        if (!narrow) {
            return;
        }
        if (!narrow_compute) {
            NoteUnboundedWrite(true, cause);
            return;
        }
        acquire_position = write_position;
        acquire_cause = cause;
        unbounded_writer_position = write_position;
        writer_cause = cause;
        if (diag) [[unlikely]] {
            ++diag_notes[static_cast<size_t>(cause)];
        }
    }
    /// EDEN_NX2_NARROW_COMPUTE: compute work is unbounded only where it launches or writes.
    [[nodiscard]] bool NarrowCompute() const {
        return narrow_compute;
    }
    /// EDEN_NX2_NARROW_RENDER_ENABLE: render_enable.mode is no unbounded write.
    [[nodiscard]] bool NarrowRenderEnable() const {
        return narrow_render_enable;
    }
    /// EDEN_NX2_NARROW_DMA_RECORDED: macro DMA reads wait only for the recorded writes.
    [[nodiscard]] bool NarrowDmaRecorded() const {
        return narrow_dma_recorded;
    }
    /// EDEN_NX2_NARROW_LIST_SYNC: GPU-written command lists wait for the work before the last
    /// host-level wait and the recorded writes over them, not for every dispatch and copy.
    [[nodiscard]] bool NarrowListSync() const {
        return narrow_list_sync;
    }
    /// EDEN_NX2_NARROW_DIAG=1.
    [[nodiscard]] bool NarrowDiag() const {
        return diag;
    }

    // Semaphore prediction (EDEN_NX2_SEM_PREDICT): the values of the releases the front queued.
    // While a release is still queued, an acquire it meets is met: the back runs the work after
    // the acquire behind the release, as the hardware would once the release lands.
    [[nodiscard]] bool SemaphorePredict() const {
        return sem_predict;
    }
    /// A release of `value` at `address` of `space` was just sent.
    void NoteRelease(const void* space, GPUVAddr address, u32 value);
    /// The value of the latest still-queued release at `address` of `space`, if any.
    [[nodiscard]] std::optional<u32> PredictedSemaphore(const void* space, GPUVAddr address);
    /// EDEN_NX2_NARROW_DIAG: an acquire check (`predicted`: met by a queued release; `waited`:
    /// it still waited for the back).
    void NoteAcquireDiag(bool predicted, bool waited) {
        if (diag) [[unlikely]] {
            ++diag_acquires[(predicted ? 2 : 0) + (waited ? 1 : 0)];
        }
    }
    /// The work just sent may draw (only EDEN_NX2_NARROW_DRAIN=2 counts it as unbounded).
    void NoteDraw() {
        if (narrow_strict) {
            unbounded_position = write_position;
            acquire_position = write_position;
        }
    }
    /// Waits until the back executed the queued work that may write [address, address + size) of
    /// `space`: the unbounded writes and the overlapping ones NoteWrite recorded. Returns whether
    /// any of it was still queued.
    bool WaitForWritesTo(const void* space, GPUVAddr address, u64 size, DrainSite site,
                         NarrowBase base = NarrowBase::Read);
    /// WaitForWriters for a command list at [address, address + size): the compute and copy work
    /// with unbounded writes, and the queued writes overlapping the list. `sync_only`
    /// (EDEN_NX2_NARROW_LIST_SYNC): the work before the last host-level wait instead of every
    /// dispatch and copy.
    bool WaitForWritersTo(const void* space, GPUVAddr address, u64 size, bool sync_only = false);

private:
    friend struct GpuSplitTestAccess;

    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        if (write_position - published_local >= publish_every) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);
    bool WaitFor(u64 target, DrainSite site);
    /// WaitFor, with EDEN_NX2_NARROW_DIAG attributing a real wait to `cause`.
    bool WaitForAttributed(u64 target, DrainSite site, UnboundedCause cause);
    void MaybeReportDiag();

    /// One step of a front wait for the back to consume `target`.
    void Backoff(u32 spins, u64 target);
    /// After the back stored `consumed`: wakes a front sleeping in Backoff.
    void NotifyFront() {
        if (front_waiting.load(std::memory_order_seq_cst)) {
            front_wake.fetch_add(1, std::memory_order_seq_cst);
            front_wake.notify_one();
        }
    }

    void BackLoop(std::stop_token stop_token);
    /// Runs the entry at `entry` (of the `words` published before the ring's end), returns its size.
    size_t Execute(const u64* entry, size_t words);

    /// Narrow drains: a queued write with a known destination, done once `consumed` reaches
    /// `position` (the end of the work that makes it).
    struct PendingWrite {
        const void* space;
        GPUVAddr begin;
        GPUVAddr end;
        u64 position;
    };
    static constexpr size_t MaxPendingWrites = 512;
    static constexpr size_t PendingWriteMask = MaxPendingWrites - 1;
    void RecordWrite(const void* space, GPUVAddr address, u64 size);
    /// Forgets the recorded writes the back executed (by consumed_cache).
    void PrunePendingWrites();
    /// The position to wait for before reading [address, address + size) of `space`: at least
    /// `base`, and past the latest recorded write that may overlap it.
    [[nodiscard]] u64 PendingWriteTarget(const void* space, GPUVAddr address, u64 size,
                                         u64 base) const;

    Core::System* system{};
    bool active{};
    bool front_macros{};
    bool narrow{};
    bool narrow_strict{};
    bool narrow_compute{};
    bool narrow_render_enable{};
    bool narrow_dma_recorded{};
    bool narrow_list_sync{};
    bool sem_predict{};
    bool diag{};
    u64 session{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 last_writer_position{};
    u64 last_sync_position{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};
    /// Narrow drains: the end of the latest work with unbounded writes (of compute or copy work).
    u64 unbounded_position{};
    u64 unbounded_writer_position{};
    /// Unbounded writes and dispatches (semaphore acquires wait for those).
    u64 acquire_position{};
    /// The latest overflowed record (NarrowBase::Recorded reads wait for it).
    u64 overflow_position{};
    UnboundedCause unbounded_cause{};
    UnboundedCause acquire_cause{};
    UnboundedCause writer_cause{};
    /// Releases the front queued (EDEN_NX2_SEM_PREDICT), round robin.
    struct QueuedRelease {
        const void* space;
        GPUVAddr address;
        u64 position;
        u32 value;
    };
    static constexpr size_t MaxQueuedReleases = 32;
    std::array<QueuedRelease, MaxQueuedReleases> queued_releases{};
    size_t next_release{};
    // EDEN_NX2_NARROW_DIAG (front only): notes per cause; real waits and their ticks per site
    // and cause; acquire checks (predicted, waited).
    std::array<u64, static_cast<size_t>(UnboundedCause::Count)> diag_notes{};
    std::array<std::array<u64, static_cast<size_t>(UnboundedCause::Count)>,
               static_cast<size_t>(DrainSite::Count)>
        diag_waits{};
    std::array<std::array<u64, static_cast<size_t>(UnboundedCause::Count)>,
               static_cast<size_t>(DrainSite::Count)>
        diag_wait_ticks{};
    std::array<u64, 4> diag_acquires{};
    u64 diag_window_start{};
    u64 diag_window_ticks{};
    /// Recorded writes not yet known done, oldest first: [pending_head, pending_tail).
    std::array<PendingWrite, MaxPendingWrites> pending_writes{};
    u64 pending_head{};
    u64 pending_tail{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    void (*idle_hook)(void*){};
    void* idle_argument{};
    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> back_running{};

    // 128 bytes: x86 spatial prefetchers pull 64-byte lines in pairs. back_waiting and wake are
    // always touched together by both threads, so they share a block.
    alignas(128) std::atomic<u64> published{};
    alignas(128) std::atomic<u64> consumed{};
    /// A position the front waits for (WaitFor): the back stores `consumed` once past it.
    alignas(128) std::atomic<u64> wait_target{~u64{0}};
    alignas(128) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    /// The front sleeps in Backoff (#161: instead of yielding).
    alignas(128) std::atomic<bool> front_waiting{};
    std::atomic<u32> front_wake{};
    std::jthread back_thread;
};

} // namespace VideoCommon
