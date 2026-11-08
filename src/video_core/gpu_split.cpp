// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <cstring>
#include <string>

#include "common/cpu_features.h"
#include "common/dkb_cpu_trace.h"
#if defined(__x86_64__) || defined(_M_X64)
#include "common/x64/rdtsc.h"
#endif
#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

/// Parks the hardware thread until `word` no longer holds `seen` or `ticks` TSC ticks passed,
/// with MONITORX/MWAITX (AMD) or UMONITOR/UMWAIT (Intel). Unlike a PAUSE loop, the core's SMT
/// sibling gets the whole core meanwhile, and a store to the word's line ends the wait at once.
/// Only when CanPark().
#if defined(__x86_64__) && (defined(__clang__) || defined(__GNUC__))
__attribute__((target("mwaitx,waitpkg")))
#endif
void Park([[maybe_unused]] const std::atomic<u64>& word, [[maybe_unused]] u64 seen,
          [[maybe_unused]] u32 ticks) {
#if defined(__x86_64__) || defined(_M_X64)
    void* const line = const_cast<std::atomic<u64>*>(&word);
    if (Common::g_cpu_caps.monitorx) {
        // Armed before the check, so a store after it ends the MWAITX.
        _mm_monitorx(line, 0, 0);
        if (word.load(std::memory_order_acquire) == seen) {
            _mm_mwaitx(1 << 1, 0, ticks); // bit 1: `ticks` bounds the wait
        }
    } else {
        _umonitor(line);
        if (word.load(std::memory_order_acquire) == seen) {
            _umwait(0, Common::X64::FencedRDTSC() + ticks); // C0.2; the deadline is absolute
        }
    }
#endif
}

bool CanPark() {
#if defined(__x86_64__) || defined(_M_X64)
    return Common::g_cpu_caps.monitorx || Common::g_cpu_caps.waitpkg;
#else
    return false;
#endif
}

/// TSC ticks a split thread parks per Park call, and how many calls before it sleeps in the OS.
/// About 200 us in all, as long as the PAUSE loops they replace (16,384 PAUSEs on the front,
/// 8,192 on the back); the back looks at hook requests between calls.
u32 ParkTicks() {
    static const u32 ticks =
        static_cast<u32>(Common::g_wall_clock.NsToTicks(std::chrono::microseconds{2}));
    return ticks;
}
constexpr u32 ParkCalls = 100;
/// PAUSEs before parking: a front publish or back store usually comes within a few hundred ns.
constexpr u32 PauseSpins = 64;

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};

/// How far ahead of the entry it runs the back prefetches ring words (16 lines).
constexpr u64 RingPrefetchWords = 128;

void PrefetchRingLine(const u64* address) {
#if defined(__clang__) || defined(__GNUC__)
    __builtin_prefetch(address, 0, 3);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(reinterpret_cast<const char*>(address), _MM_HINT_T0);
#else
    (void)address;
#endif
}

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    // Also while the back is idle: its idle hook submits (Scheduler::FlushDeferred).
    if (!instance.back_running.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::string stack;
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        stack += fmt::format(" {}+{:#x}", base_name ? base_name + 1 : name,
                             reinterpret_cast<uintptr_t>(frames[i]) -
                                 reinterpret_cast<uintptr_t>(module));
    }
    LOG_ERROR(HW_GPU, "GPU split owner check: {} on thread {} while the back runs; stack:{}",
              where, thread, stack);
#else
    (void)where;
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
    ++starts;
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
    if (front_macros) {
        page_slots = std::make_unique<PageSlot[]>(PageBuckets * 4);
        word_slots = std::make_unique<WordSlot[]>(WordBuckets * 4);
        queued_releases = std::make_unique<QueuedRelease[]>(ReleaseBuckets * 4);
    }
    ++session;
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros and narrow drains on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    active = false;
    ForgetFencesQuiet();
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    last_writer_position = 0;
    pending_write_count = 0;
    async_writes.clear();
    fence_writes.clear();
    report_pages.clear();
    last_sync_position = 0;
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    pending_flush = nullptr;
    pending_flush_argument = nullptr;
    back_engine = nullptr;
    front_macros = false;
    for (auto& page : gpu_semaphore_pages) {
        page.store(0, std::memory_order_relaxed);
    }
    gpu_semaphore_count.store(0, std::memory_order_relaxed);
    gpu_semaphore_saturated.store(false, std::memory_order_relaxed);
    unbounded_position = 0;
    unbounded_writer_position = 0;
    acquire_position = 0;
    write_floor = 0;
    guest_write_position = 0;
    release_barrier = 0;
    queued_releases.reset();
    page_slots.reset();
    word_slots.reset();
    huge_writes = {};
    next_huge = 0;
    spaces = {};
    space_last = {};
    used_spaces = 0;
    last_record = 0;
    last_page_slot = nullptr;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
    back_idle.store(false, std::memory_order_relaxed);
    front_waiting.store(false, std::memory_order_relaxed);
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            return;
        }
        Backoff(spins, write_position + words - RingWords);
    }
}

void GpuSplit::Publish() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    // Taken back here: the publishes until the back runs again don't wake it once more each.
    if (back_waiting.load(std::memory_order_seq_cst) &&
        back_waiting.exchange(false, std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

void GpuSplit::Drain(DrainSite site) {
    RunPendingFlush();
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position && BackQuiet()) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Ticks() : 0;
    // Everything consumed is not enough: the back submits deferred work (Scheduler::DeferFlush)
    // from its idle hook afterwards, and the caller may use the scheduler once this returns.
    bool hook_asked = false;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            if (BackQuiet()) {
                publish_every = 8;
                Nx2Profile::AddDrain(site, start);
                return;
            }
            if (!hook_asked) {
                RequestIdleHook();
                hook_asked = true;
            }
        }
        Backoff(spins, write_position);
    }
}

bool GpuSplit::WaitForWriters(DrainSite site) {
    // Held-back work goes first: a writer in it is noted before the target is taken.
    RunPendingFlush();
    return WaitFor(std::min(last_writer_position, write_position), site);
}

bool GpuSplit::WaitForWritesTo(u64 begin, u64 size) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    const u64 end = begin + size;
    const u64 remembered = (std::min<u64>)(pending_write_count, pending_writes.size());
    u64 target = 0;
    u64 scanned = 0;
    for (; scanned < remembered; ++scanned) {
        const PendingWrite& write =
            pending_writes[(pending_write_count - 1 - scanned) % pending_writes.size()];
        if (write.position <= consumed_cache) {
            break;
        }
        if (write.begin < end && begin < write.end) {
            target = (std::max)(target, write.position);
        }
    }
    if (scanned == pending_writes.size()) {
        return false;
    }
    if (target != 0) {
        WaitFor(target, DrainSite::MacroDmaRead);
    }
    return true;
}

bool GpuSplit::OverlayQueuedWrites(u64 since, u64 begin, u32* out, u32 words) {
    if ((begin & 3) != 0) {
        return false;
    }
    const u64 end = begin + u64{words} * sizeof(u32);
    const u64 remembered = (std::min<u64>)(pending_write_count, pending_writes.size());
    // Newest first, back to what the back had consumed before guest memory was read.
    std::array<u32, 64> overlapping;
    u32 found = 0;
    u64 scanned = 0;
    for (; scanned < remembered; ++scanned) {
        const u64 index = (pending_write_count - 1 - scanned) % pending_writes.size();
        const PendingWrite& write = pending_writes[index];
        if (write.position <= since) {
            break;
        }
        if (write.begin < end && begin < write.end) {
            if (write.words == 0 || (write.begin & 3) != 0 || found == overlapping.size()) {
                return false;
            }
            overlapping[found++] = static_cast<u32>(index);
        }
    }
    if (scanned == pending_writes.size()) {
        return false;
    }
    for (u32 i = found; i-- > 0;) {
        const PendingWrite& write = pending_writes[overlapping[i]];
        for (u32 w = 0; w < write.words; ++w) {
            const u64 address = write.begin + u64{w} * sizeof(u32);
            if (address >= begin && address < end) {
                out[(address - begin) / sizeof(u32)] = write.data[w];
            }
        }
    }
    return true;
}

bool GpuSplit::WaitForSync() {
    RunPendingFlush();
    return WaitFor(std::min(last_sync_position, write_position), DrainSite::ListSync);
}

bool GpuSplit::NarrowVerify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_NARROW_VERIFY");
        return value != nullptr && value[0] != '0';
    }();
    return verify;
}

void GpuSplit::ReportNarrowCheck(DrainSite site, u32 mismatches, u32 macro, GPUVAddr address) {
    struct Counts {
        std::atomic<u64> checks;
        std::atomic<u64> mismatched;
    };
    static std::array<Counts, static_cast<size_t>(DrainSite::Count)> counts{};
    Counts& count = counts[static_cast<size_t>(site)];
    const u64 checks = count.checks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (mismatches != 0) {
        const u64 mismatched = count.mismatched.fetch_add(1, std::memory_order_relaxed) + 1;
        if (mismatched <= 32 || (mismatched & 1023) == 0) {
            LOG_WARNING(HW_GPU,
                        "Narrow drain verify: {} at {:#x} (macro {:#x}) read {} words other than "
                        "a full drain; {} of {} checks differed so far",
                        drain_site_names[static_cast<size_t>(site)], address, macro, mismatches,
                        mismatched, checks);
        }
    }
    if ((checks & 0xffff) == 0) {
        LOG_INFO(HW_GPU, "Narrow drain verify: {}: {} checks, {} differed",
                 drain_site_names[static_cast<size_t>(site)], checks,
                 count.mismatched.load(std::memory_order_relaxed));
    }
}

namespace {

[[nodiscard]] size_t HashBucket(u64 key, size_t buckets) {
    return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 32) & (buckets - 1);
}

// Whether NoteGpuSemaphore saw `page` (or can't tell).
bool GpuSemaphorePage(const std::atomic<u64>* pages, size_t slots, u64 page) {
    const u64 key = page + 1;
    size_t index = HashBucket(key, slots);
    for (size_t probe = 0; probe < 16; ++probe, index = (index + 1) & (slots - 1)) {
        const u64 current = pages[index].load(std::memory_order_acquire);
        if (current == key) {
            return true;
        }
        if (current == 0) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

void GpuSplit::NoteGpuSemaphore(GPUVAddr address) {
    if (gpu_semaphore_saturated.load(std::memory_order_relaxed)) {
        return;
    }
    const u64 key = (address >> PageBits) + 1;
    size_t index = HashBucket(key, GpuSemaphoreSlots);
    for (size_t probe = 0; probe < 16; ++probe, index = (index + 1) & (GpuSemaphoreSlots - 1)) {
        u64 current = gpu_semaphore_pages[index].load(std::memory_order_acquire);
        if (current == key) {
            return;
        }
        if (current == 0) {
            if (gpu_semaphore_pages[index].compare_exchange_strong(current, key,
                                                                   std::memory_order_acq_rel)) {
                if (gpu_semaphore_count.fetch_add(1, std::memory_order_relaxed) + 1 >=
                    GpuSemaphoreSlots * 3 / 4) {
                    gpu_semaphore_saturated.store(true, std::memory_order_release);
                }
                return;
            }
            if (current == key) {
                return;
            }
        }
    }
    gpu_semaphore_saturated.store(true, std::memory_order_release);
}

bool GpuSplit::AcquireSyncs(GPUVAddr address) const {
    if (gpu_semaphore_saturated.load(std::memory_order_acquire)) {
        return true;
    }
    return GpuSemaphorePage(gpu_semaphore_pages.data(), GpuSemaphoreSlots, address >> PageBits);
}

bool GpuSplit::MayHoldReports(GPUVAddr address, u64 size) {
    if (size == 0) {
        return false;
    }
    if (gpu_semaphore_saturated.load(std::memory_order_acquire)) {
        return true;
    }
    for (GPUVAddr page = address >> PageBits; page <= (address + size - 1) >> PageBits; ++page) {
        if (GpuSemaphorePage(gpu_semaphore_pages.data(), GpuSemaphoreSlots, page)) {
            return true;
        }
    }
    return false;
}

u32 GpuSplit::SpaceId(const void* space) {
    for (u32 i = 0; i < used_spaces; ++i) {
        if (spaces[i] == space) {
            return i;
        }
    }
    if (used_spaces < MaxSpaces - 1) {
        spaces[used_spaces] = space;
        return used_spaces++;
    }
    // The rest share the last id: their reads wait for all of its writes.
    return MaxSpaces - 1;
}

void GpuSplit::InsertPage(u64 key, bool coarse) {
    PageSlot* slot = last_page_slot;
    if (slot == nullptr || slot->key != key) {
        PageSlot* const bucket = &page_slots[HashBucket(key, PageBuckets) * 4];
        slot = nullptr;
        for (size_t i = 0; i < 4; ++i) {
            if (bucket[i].key == key) {
                slot = &bucket[i];
                break;
            }
        }
        if (slot == nullptr) {
            // A done slot, else the oldest (the floor waits for it).
            PageSlot* victim = &bucket[0];
            for (size_t i = 0; i < 4; ++i) {
                if (bucket[i].any <= consumed_cache) {
                    victim = &bucket[i];
                    break;
                }
                if (bucket[i].any < victim->any) {
                    victim = &bucket[i];
                }
            }
            RaiseFloor(victim->any);
            *victim = {key, 0, 0};
            slot = victim;
        }
        last_page_slot = slot;
    }
    if (coarse) {
        slot->coarse = write_position;
    }
    slot->any = write_position;
}

void GpuSplit::InsertWord(u64 key) {
    WordSlot* const bucket = &word_slots[HashBucket(key, WordBuckets) * 4];
    WordSlot* victim = nullptr;
    for (size_t i = 0; i < 4; ++i) {
        if (bucket[i].key == key) {
            bucket[i].position = write_position;
            return;
        }
        if (victim == nullptr && bucket[i].position <= consumed_cache) {
            victim = &bucket[i];
        }
    }
    if (victim == nullptr) {
        victim = &bucket[0];
        for (size_t i = 1; i < 4; ++i) {
            if (bucket[i].position < victim->position) {
                victim = &bucket[i];
            }
        }
        RaiseFloor(victim->position);
    }
    *victim = {key, write_position};
}

const GpuSplit::PageSlot* GpuSplit::FindPage(u64 key) const {
    const PageSlot* const bucket = &page_slots[HashBucket(key, PageBuckets) * 4];
    for (size_t i = 0; i < 4; ++i) {
        if (bucket[i].key == key) {
            return &bucket[i];
        }
    }
    return nullptr;
}

const GpuSplit::WordSlot* GpuSplit::FindWord(u64 key) const {
    const WordSlot* const bucket = &word_slots[HashBucket(key, WordBuckets) * 4];
    for (size_t i = 0; i < 4; ++i) {
        if (bucket[i].key == key) {
            return &bucket[i];
        }
    }
    return nullptr;
}

void GpuSplit::RecordWrite(const void* space, GPUVAddr address, u64 size, bool fine) {
    if (page_slots == nullptr) {
        return;
    }
    const u64 sid = SpaceId(space);
    space_last[sid] = write_position;
    last_record = write_position;
    const GPUVAddr end = address + size;
    const GPUVAddr first_page = address >> PageBits;
    const GPUVAddr last_page = (end - 1) >> PageBits;
    if (fine) {
        NoteGpuSemaphore(address);
    }
    if (fine && size <= MaxFineBytes) {
        for (GPUVAddr word = address >> WordBits; word <= (end - 1) >> WordBits; ++word) {
            InsertWord((word << 2) | sid);
        }
        for (GPUVAddr page = first_page; page <= last_page; ++page) {
            InsertPage((page << 2) | sid, false);
        }
        return;
    }
    if (last_page - first_page >= 64) {
        // Round robin replaces the oldest.
        HugeWrite& slot = huge_writes[next_huge];
        RaiseFloor(slot.position);
        slot = {space, address, end, write_position};
        next_huge = (next_huge + 1) % MaxHugeWrites;
        return;
    }
    for (GPUVAddr page = first_page; page <= last_page; ++page) {
        InsertPage((page << 2) | sid, true);
    }
}

u64 GpuSplit::PendingWriteTarget(const void* space, GPUVAddr address, u64 size, u64 base) const {
    u64 target = (std::max)(base, write_floor);
    if (page_slots == nullptr || size == 0) {
        return target;
    }
    const GPUVAddr end = address + size;
    u64 sid = MaxSpaces;
    for (u32 i = 0; i < used_spaces; ++i) {
        if (spaces[i] == space) {
            sid = i;
            break;
        }
    }
    // Another address space may map the same memory.
    for (u64 i = 0; i < MaxSpaces; ++i) {
        if (i != sid || i == MaxSpaces - 1) {
            target = (std::max)(target, space_last[i]);
        }
    }
    for (const HugeWrite& write : huge_writes) {
        if (write.position > target &&
            (write.space != space || (write.begin < end && address < write.end))) {
            target = write.position;
        }
    }
    if (sid == MaxSpaces) {
        // Nothing recorded in this space itself.
        return target;
    }
    const GPUVAddr first_page = address >> PageBits;
    const GPUVAddr last_page = (end - 1) >> PageBits;
    if (last_page - first_page >= 64) {
        return (std::max)(target, last_record);
    }
    // By the word only for small reads; `small` itself is a macro in the Windows SDK (rpcndr.h).
    const bool word_granular = size <= 256;
    for (GPUVAddr page = first_page; page <= last_page; ++page) {
        const PageSlot* const slot = FindPage((page << 2) | sid);
        if (slot == nullptr || slot->any <= (std::max)(target, consumed_cache)) {
            // No pending write in the page (a pending slot that gave way raised the floor).
            continue;
        }
        if (!word_granular) {
            target = slot->any;
            continue;
        }
        target = (std::max)(target, slot->coarse);
        const GPUVAddr page_begin = (std::max)(address, page << PageBits);
        const GPUVAddr page_end = (std::min)(end, (page + 1) << PageBits);
        for (GPUVAddr word = page_begin >> WordBits; word <= (page_end - 1) >> WordBits; ++word) {
            if (const WordSlot* const word_slot = FindWord((word << 2) | sid)) {
                target = (std::max)(target, word_slot->position);
            }
        }
    }
    return target;
}

bool GpuSplit::WrittenSince(const void* space, GPUVAddr address, u64 position) const {
    if (page_slots == nullptr || write_floor > position) {
        return true;
    }
    u64 sid = MaxSpaces;
    for (u32 i = 0; i < used_spaces; ++i) {
        if (spaces[i] == space) {
            sid = i;
            break;
        }
    }
    if (sid == MaxSpaces) {
        return true;
    }
    for (u64 i = 0; i < MaxSpaces; ++i) {
        if ((i != sid || i == MaxSpaces - 1) && space_last[i] > position) {
            return true;
        }
    }
    for (const HugeWrite& write : huge_writes) {
        if (write.position > position && write.begin <= address && address < write.end) {
            return true;
        }
    }
    // A slot replaced since no longer tells: count it as written.
    const PageSlot* const page = FindPage(((address >> PageBits) << 2) | sid);
    if (page == nullptr || page->coarse > position) {
        return true;
    }
    const WordSlot* const word = FindWord(((address >> WordBits) << 2) | sid);
    return word == nullptr || word->position > position;
}

GpuSplit::QueuedRelease* GpuSplit::FindRelease(const void* space, GPUVAddr address) {
    QueuedRelease* const bucket =
        &queued_releases[HashBucket(address >> WordBits, ReleaseBuckets) * 4];
    for (size_t i = 0; i < 4; ++i) {
        if (bucket[i].used && bucket[i].address == address && bucket[i].space == space) {
            return &bucket[i];
        }
    }
    return nullptr;
}

void GpuSplit::NoteRelease(const void* space, GPUVAddr address, u32 value, bool at_least) {
    if (queued_releases == nullptr) {
        return;
    }
    const auto* const memory = static_cast<const Tegra::MemoryManager*>(space);
    const auto device = memory->GpuToCpuAddress(address);
    // Unmapped (Ocarina releases to address 0 at boot): the read would return 0 anyway, and log
    // an error for each release.
    const u32 before = device ? memory->Read<u32>(address) : 0;
    QueuedRelease* release = FindRelease(space, address);
    if (release == nullptr) {
        // A free slot, else the oldest.
        QueuedRelease* const bucket =
            &queued_releases[HashBucket(address >> WordBits, ReleaseBuckets) * 4];
        release = &bucket[0];
        for (size_t i = 0; i < 4; ++i) {
            if (!bucket[i].used) {
                release = &bucket[i];
                break;
            }
            if (bucket[i].position < release->position) {
                release = &bucket[i];
            }
        }
        release->used = false;
    }
    const u32 slot = device ? static_cast<u32>(Tegra::Nx2LandedSlot(*device)) : 0;
    const u32 landed = Tegra::g_nx2_semaphore_landed[slot].load(std::memory_order_acquire);
    if (!release->used || release->landed_slot != slot ||
        landed - release->landed_base >= release->sent) {
        // The releases before it landed: count from here.
        release->landed_base = landed;
        release->sent = 0;
    }
    release->space = space;
    release->address = address;
    release->position = write_position;
    release->value = value;
    release->before = before;
    release->landed_slot = slot;
    release->sent += device ? 1 : 0;
    release->used = true;
    release->at_least = at_least;
}

std::optional<SemaphorePrediction> GpuSplit::PredictedRelease(const void* space, GPUVAddr address) {
    if (queued_releases == nullptr) {
        return std::nullopt;
    }
    consumed_cache = consumed.load(std::memory_order_acquire);
    const QueuedRelease* const release = FindRelease(space, address);
    // A write sent after the release (a copy, a compute report) may change the word; so may
    // work the front lost track of (NoteReleaseBarrier).
    if (release == nullptr || release->position < release_barrier ||
        WrittenSince(space, address, release->position)) {
        return std::nullopt;
    }
    const SemaphorePrediction prediction{
        .value = release->value,
        .at_least = release->at_least,
        .position = release->position,
    };
    if (release->position > consumed_cache) {
        return prediction;
    }
    if (release->sent == 0) {
        return std::nullopt;
    }
    // The back ran it. A fence may still hold the write back (one-word releases land when the
    // fence after them signals): while it hasn't landed and guest memory still holds the word
    // from before it, the word is the release's.
    const u32 landed =
        Tegra::g_nx2_semaphore_landed[release->landed_slot].load(std::memory_order_acquire);
    if (landed - release->landed_base >= release->sent) {
        return std::nullopt;
    }
    const u32 now = static_cast<const Tegra::MemoryManager*>(space)->Read<u32>(address);
    if (now != release->before && now != release->value) {
        return std::nullopt;
    }
    return prediction;
}

void GpuSplit::ForgetRelease(const void* space, GPUVAddr address) {
    if (queued_releases == nullptr) {
        return;
    }
    if (QueuedRelease* const release = FindRelease(space, address)) {
        release->used = false;
    }
}

bool GpuSplit::WaitForQueuedWritesTo(const void* space, GPUVAddr address, u64 size, DrainSite site,
                                     NarrowBase base_kind) {
    // Held-back writes (constant buffer runs) are sent and recorded first.
    RunPendingFlush();
    consumed_cache = consumed.load(std::memory_order_acquire);
    u64 base = unbounded_position;
    if (base_kind == NarrowBase::Acquire) {
        base = acquire_position;
    } else if (base_kind == NarrowBase::Recorded) {
        base = 0;
    }
    const u64 target = PendingWriteTarget(space, address, size, base);
    return WaitFor((std::min)(target, write_position), site);
}

bool GpuSplit::WaitForWritersTo(const void* space, GPUVAddr address, u64 size, bool all_writers) {
    RunPendingFlush(); // As in WaitForQueuedWritesTo
    consumed_cache = consumed.load(std::memory_order_acquire);
    const u64 base = all_writers ? unbounded_writer_position : last_sync_position;
    const u64 target = PendingWriteTarget(space, address, size, base);
    return WaitFor((std::min)(target, write_position), DrainSite::NarrowListWriters);
}

bool GpuSplit::WaitFor(u64 target, DrainSite site) {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        return false;
    }
    // The back gets the held-back work too, so it has that to run once it reaches the target
    // instead of waiting for the front.
    RunPendingFlush();
    Nx2Profile::Scope scope{Nx2Profile::Zone::WriterWait};
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    const u64 start = Nx2Profile::Enabled() ? Nx2Profile::Ticks() : 0;
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            wait_target.store(~u64{0}, std::memory_order_relaxed);
            Nx2Profile::AddDrain(site, start);
            return true;
        }
        Backoff(spins, target);
    }
}

void GpuSplit::Backoff(u32 spins, u64 target) {
    if (spins < PauseSpins) {
        Pause();
        return;
    }
    if (CanPark()) {
        if (spins < PauseSpins + ParkCalls) {
            const u64 seen = consumed.load(std::memory_order_acquire);
            if (seen < target) {
                Park(consumed, seen, ParkTicks());
            }
            return;
        }
    } else if (spins < 16384) {
        Pause();
        return;
    }
    if (!back_running.load(std::memory_order_acquire)) {
        std::this_thread::yield();
        return;
    }
    // A long wait sleeps until the back stores a new position (port of #161's front_wake): the
    // back checks front_waiting after each store, the front checks the position after setting it.
    front_waiting.store(true, std::memory_order_seq_cst);
    const u32 seen = front_wake.load(std::memory_order_seq_cst);
    if (consumed.load(std::memory_order_seq_cst) < target) {
        front_wake.wait(seen, std::memory_order_seq_cst);
    }
    front_waiting.store(false, std::memory_order_relaxed);
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    // Before front_engine changes: the held-back work selects its own engine.
    RunPendingFlush();
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry, size_t words) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        Nx2Profile::MethodScope method_scope{static_cast<u32>(engine->engine_type), method};
        if (engine->fast_maxwell3d) {
            // Nx2Perf: as below without the virtual call.
            using Tegra::Engines::Maxwell3D;
            auto* const maxwell3d = static_cast<Maxwell3D*>(engine);
            constexpr u32 cb_data = MAXWELL3D_REG_INDEX(const_buffer.buffer);
            if (method - cb_data < 16 && (head & HasSegment) == 0) {
                // Single constant buffer words in a row (macros write them one by one) go up in
                // one write. They are executable: each entry would only consume the sink first.
                std::array<u32, 64> methods;
                std::array<u32, 64> arguments;
                methods[0] = method;
                arguments[0] = argument;
                u32 count = 1;
                while (count < methods.size() && count < words) {
                    const u64 next = entry[count];
                    const u32 next_method = static_cast<u32>((next >> 8) & 0xffff);
                    if (static_cast<Kind>(next & 0xf) != Kind::Method || (next & HasSegment) != 0 ||
                        next_method - cb_data >= 16) {
                        break;
                    }
                    methods[count] = next_method;
                    arguments[count] = static_cast<u32>(next >> 32);
                    ++count;
                }
                maxwell3d->ConsumeSink(*system);
                if (count == 1) {
                    maxwell3d->CallMethod(*system, method, argument, true);
                } else {
                    maxwell3d->CallConstBufferWords(*system, methods.data(), arguments.data(),
                                                    count);
                }
                return count;
            }
            if ((head & Direct) != 0) {
                maxwell3d->ConsumeSink(*system);
                maxwell3d->CallMethod(*system, method, argument, true);
            } else if (!maxwell3d->execution_mask[method]) {
                maxwell3d->method_sink.emplace_back(method, argument);
            } else {
                maxwell3d->ConsumeSink(*system);
                if ((head & HasSegment) != 0) {
                    maxwell3d->current_dma_segment = entry[1];
                }
                maxwell3d->CallMethod(*system, method, argument, (head & IsLast) != 0);
            }
            return (head & HasSegment) != 0 ? 2 : 1;
        }
        if ((head & Direct) != 0) {
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::MethodScope method_scope{static_cast<u32>(engine->engine_type), method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    case Kind::StateDelta: {
        // Only Maxwell3D sends these (FrontState).
        const u32 count = static_cast<u32>(head >> 32);
        {
            Nx2Profile::Scope delta_scope{Nx2Profile::Zone::DeltaApply};
            static_cast<Tegra::Engines::Maxwell3D*>(back_engine)
                ->ApplyFrontDelta(*system, entry + 1, count);
        }
        const size_t size = 1 + size_t{count};
        // The front sends a delta right before the entry that made it go (FrontFlushPending,
        // from Reserve): run that entry now when it is published too, as BackLoop would next.
        // Only kinds that never return 0 (Wrap) or chain again.
        if (size < words) {
            const Kind next = static_cast<Kind>(entry[size] & 0xf);
            if (next == Kind::Method || next == Kind::CbWrite || next == Kind::CbBind ||
                next == Kind::DrawBatch || next == Kind::DrawRun) {
                return size + Execute(entry + size, words - size);
            }
        }
        return size;
    }
    case Kind::DrawBatch: {
        const u32 count = static_cast<u32>((head >> 8) & 0xff);
        const u32 repeats = static_cast<u32>(head >> 32);
        Nx2Profile::Scope batch_scope{Nx2Profile::Zone::DrawBatch};
        static_cast<Tegra::Engines::Maxwell3D*>(back_engine)
            ->RunDrawBatch(*system, entry + 1, count, repeats);
        return 1 + size_t{count};
    }
    case Kind::DrawRun: {
        const u32 count = static_cast<u32>(head >> 32);
        Nx2Profile::Scope run_scope{Nx2Profile::Zone::DrawRun};
        static_cast<Tegra::Engines::Maxwell3D*>(back_engine)->RunDrawRun(*system, entry + 1, count);
        return 1 + size_t{count};
    }
    case Kind::CbWrite: {
        const u32 count = static_cast<u32>((head >> 8) & 7);
        Nx2Profile::Scope cb_scope{Nx2Profile::Zone::CbWrite};
        // The words after the first (at most 3, see GpuSplit::CbWrite) stay in the ring words
        // after the head.
        static_cast<Tegra::Engines::Maxwell3D*>(back_engine)
            ->BackCbWrite(*system, static_cast<u32>((head >> 12) & 0xffff),
                          static_cast<u32>(head >> 32), entry + 1, count);
        return 1 + size_t{count} / 2;
    }
    case Kind::Writes: {
        // Only KeplerCompute sends these (FrontQueueWrite).
        const u32 count = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 2 : 1;
        Nx2Profile::Scope writes_scope{Nx2Profile::Zone::ComputeWrites};
        if ((head & HasSegment) != 0) {
            back_engine->current_dma_segment = entry[1];
        }
        if (Nx2Profile::Enabled()) [[unlikely]] {
            Nx2Profile::Add(Nx2Profile::Counter::FusedWrites, count);
        }
        static_cast<Tegra::Engines::KeplerCompute*>(back_engine)
            ->BackWrites(*system, entry + data, count);
        return data + size_t{count};
    }
    case Kind::CbBind: {
        Nx2Profile::Scope bind_scope{Nx2Profile::Zone::CbBind};
        static_cast<Tegra::Engines::Maxwell3D*>(back_engine)
            ->BackCbBind(*system, static_cast<u32>((head >> 8) & 0xff),
                         static_cast<u32>(head >> 32));
        return 1;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    const Common::DkbCpuTrace::Registration cpu_trace{Common::DkbCpuTrace::Role::GpuBack};
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    if (Settings::values.pin_gpu_threads.GetValue()) {
        Common::PinCurrentThread(Common::PinnedRole::GpuBack);
    }
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    // Counts across batches, so a stream of short batches still checks hook requests.
    u32 entries = 0;
    const bool profiling = Nx2Profile::Enabled();
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            back_idle.store(true, std::memory_order_relaxed);
            // The front usually sends more within microseconds: wait briefly before sleeping,
            // parked once a short PAUSE spin found nothing.
            const bool park = CanPark();
            const u32 waits = park ? PauseSpins + ParkCalls : 8192;
            for (u32 spins = 0; spins < waits && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                if (park && spins >= PauseSpins) {
                    Park(published, available, ParkTicks());
                } else {
                    Pause();
                }
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                hook_requested.store(false, std::memory_order_seq_cst);
                // The front may already see everything consumed: Drain() waits for back_quiet
                // too, so the deferred submit can't overlap whatever the front does next.
                if (!back_quiet.load(std::memory_order_relaxed)) {
                    if (idle_hook != nullptr) {
                        ForgetFencesQuiet();
                        Nx2Profile::Scope hook_scope{Nx2Profile::Zone::IdleHook};
                        idle_hook(idle_argument);
                    }
                    back_quiet.store(true, std::memory_order_release);
                }
                available = published.load(std::memory_order_acquire);
                if (available == read_position) {
                    CheckFencesQuiet(read_position);
                }
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                // A request made after the hook ran (RequestIdleHook) runs it before sleeping.
                if (available == read_position && !stop_token.stop_requested() &&
                    !hook_requested.load(std::memory_order_seq_cst)) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        if (back_idle.load(std::memory_order_relaxed)) {
            back_idle.store(false, std::memory_order_relaxed);
        }
        Nx2Profile::Scope ring_scope{Nx2Profile::Zone::RingDecode};
        u64 last_store = read_position;
        // Before any entry runs, so a front that sees these entries consumed also sees this.
        back_quiet.store(false, std::memory_order_seq_cst);
        // Prefetch the ring lines ahead of the entry, up to a line short of what the front
        // published (a line it may still be writing would only bounce between the two cores). The
        // front wrote them moments ago: they sit in its core's caches.
        u64 prefetched = read_position;
        while (read_position != available) {
            const size_t index = read_position & RingMask;
            const u64 ahead = (std::min)(read_position + RingPrefetchWords,
                                         available > 8 ? available - 8 : u64{0});
            prefetched = (std::max)(prefetched, read_position);
            while (prefetched + 8 <= ahead) {
                prefetched += 8;
                PrefetchRingLine(&ring[prefetched & RingMask]);
            }
            const size_t size =
                Execute(&ring[index], std::min<u64>(available - read_position, RingWords - index));
            const size_t advance = size != 0 ? size : RingWords - index;
            read_position += advance;
            if (profiling) [[unlikely]] {
                Nx2Profile::Add(Nx2Profile::Counter::RingEntries, size != 0 ? 1 : 0);
                Nx2Profile::Add(Nx2Profile::Counter::RingWords, advance);
            }
            // The hook request (another thread's cache line) every 4 entries.
            if ((++entries & 3) == 0 && hook_requested.load(std::memory_order_relaxed) &&
                idle_hook != nullptr) [[unlikely]] {
                hook_requested.store(false, std::memory_order_seq_cst);
                ForgetFencesQuiet();
                Nx2Profile::Scope hook_scope{Nx2Profile::Zone::IdleHook};
                idle_hook(idle_argument);
            }
            // Hand ring space back now and then while working through a long batch, and at once
            // when the front waits for this position (WaitForWriters).
            if ((read_position - last_store >= 4096 ||
                 read_position >= wait_target.load(std::memory_order_relaxed)) &&
                read_position != available) {
                consumed.store(read_position, std::memory_order_seq_cst);
                last_store = read_position;
                NotifyFront();
            }
        }
        CheckFencesQuiet(read_position);
        consumed.store(read_position, std::memory_order_seq_cst);
        NotifyFront();
    }
    back_running.store(false, std::memory_order_seq_cst);
    front_wake.fetch_add(1, std::memory_order_seq_cst);
    front_wake.notify_all();
}

} // namespace VideoCommon
