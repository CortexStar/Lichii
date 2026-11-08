// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define NX2_BUFCACHE_SSE2 1
#endif

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/buffer_cache/word_manager.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon::Nx2BufCache {

/// Whether [a, a + size) and [b, b + size) hold the same bytes. Inline (memcmp is an out-of-line
/// ucrt call that clang does not expand for 4 KiB), 128 bytes per step with an early exit.
[[nodiscard]] inline bool BytesEqual(const u8* a, const u8* b, size_t size) {
#ifdef NX2_BUFCACHE_SSE2
    size_t offset = 0;
    for (; offset + 128 <= size; offset += 128) {
        const auto load = [&](const u8* base, size_t index) {
            return _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + offset + index * 16));
        };
        __m128i acc = _mm_xor_si128(load(a, 0), load(b, 0));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 1), load(b, 1)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 2), load(b, 2)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 3), load(b, 3)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 4), load(b, 4)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 5), load(b, 5)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 6), load(b, 6)));
        acc = _mm_or_si128(acc, _mm_xor_si128(load(a, 7), load(b, 7)));
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(acc, _mm_setzero_si128())) != 0xFFFF) {
            return false;
        }
    }
    return offset == size || std::memcmp(a + offset, b + offset, size - offset) == 0;
#else
    return std::memcmp(a, b, size) == 0;
#endif
}

/// Whether one range of `set` covers [addr, addr + size) (interval sets join touching ranges).
template <typename Set, typename Addr>
[[nodiscard]] bool RangeCovered(const Set& set, Addr addr, u64 size) {
    if (size == 0) {
        return false;
    }
    u32 pieces = 0;
    bool covered = false;
    set.ForEachInRange(addr, size, [&](Addr lower, Addr upper) {
        ++pieces;
        covered = lower == addr && upper == addr + size;
    });
    return pieces == 1 && covered;
}

/// Device addresses seen this frame. clear() forgets them all without touching the table.
class FrameAddressSet {
public:
    /// True when `key` (non-zero) was not in the set this frame; it is now.
    bool insert(u64 key) {
        if (count * 2 >= table.size()) {
            Grow();
        }
        Entry& entry = Probe(key);
        if (entry.stamp == stamp && entry.key == key) {
            return false;
        }
        entry = Entry{key, stamp};
        ++count;
        return true;
    }

    [[nodiscard]] bool contains(u64 key) const {
        if (table.empty()) {
            return false;
        }
        const size_t mask = table.size() - 1;
        for (size_t index = Hash(key) & mask;; index = (index + 1) & mask) {
            const Entry& entry = table[index];
            if (entry.stamp != stamp) {
                return false;
            }
            if (entry.key == key) {
                return true;
            }
        }
    }

    void clear() {
        count = 0;
        if (++stamp == 0) {
            for (Entry& entry : table) {
                entry.stamp = 0;
            }
            stamp = 1;
        }
    }

private:
    struct Entry {
        u64 key{};
        u32 stamp{};
    };

    static size_t Hash(u64 key) {
        return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 32);
    }

    Entry& Probe(u64 key) {
        const size_t mask = table.size() - 1;
        for (size_t index = Hash(key) & mask;; index = (index + 1) & mask) {
            Entry& entry = table[index];
            if (entry.stamp != stamp || entry.key == key) {
                return entry;
            }
        }
    }

    void Grow() {
        std::vector<Entry> old = std::move(table);
        table.assign(old.empty() ? 256 : old.size() * 2, Entry{});
        const u32 live = stamp;
        stamp = 1;
        count = 0;
        for (const Entry& entry : old) {
            if (entry.stamp == live) {
                Probe(entry.key) = Entry{entry.key, stamp};
                ++count;
            }
        }
    }

    std::vector<Entry> table;
    size_t count{};
    u32 stamp{1};
};

/// Buffer cache paths taken, logged every 10 s (from TickFrame) with EDEN_NX2_GPU_PROFILE.
enum class Stat : u32 {
    UboStream,
    UboClassic,
    UboClassicUpload,
    BankUpload,
    BankUploadBytes,
    BankSkip,
    SyncUpload,
    SyncUploadBytes,
    ResolveContig,
    ResolveQuery,
    MarkWritten,
    MarkAddSkipped,
    InlineFast,
    InlineSlow,
    HotPromote,
    HotDemote,
    HotSame,
    HotUpload,
    HotCostDemote,
    SyncSkipHit,
    SyncSkipHotHit,
    SyncWalk,
    StorageMemoHit,
    UboRebindKept,
    UboSlotHit,     ///< stream: the slot's copy bound again (same bytes, same command buffer)
    UboRangeHit,    ///< stream: another bind's copy of the range bound again
    UboRecopied,    ///< stream: same bytes as an older command buffer's copy, copied from it
    UboCopied,      ///< stream: guest bytes copied
    UboCopiedBytes, ///< bytes of UboRecopied and UboCopied
    UboLarge,       ///< stream binds over 4 KiB (misaligned buffers): copied every time
    UboLargeBytes,
    SyncPrecheckClean, ///< syncs a SYNC_SKIP miss sent to the walk, found clean by the precheck
    WriteMemoryCalls,  ///< BufferCache::WriteMemory calls
    DirtyWriteSkip,    ///< of those, over pages all CPU-dirty already
    UboClassicBank,    ///< classic binds of a native 64 KiB bank
    UboClassicSmall,   ///< classic binds within the stream size (GPU-written, or no host buffer)
    BankCompare,       ///< UploadNativeBank calls (once a frame per bank)
    BankComparePages,  ///< bank pages compared with their shadow
    BankPageRepeat,    ///< of those, pages another bank already compared this frame
    BankShadowDrop,    ///< bank shadows dropped at the cap (the least recently compared)
    BankSyncRefresh,   ///< bank shadow pages a sync upload refreshed
    Count,
};

inline std::array<std::atomic<u64>, static_cast<size_t>(Stat::Count)> g_stats{};
inline u64 g_stats_start{};

inline void Note(Stat stat, u64 amount = 1) {
    if (Nx2Profile::Enabled()) {
        g_stats[static_cast<size_t>(stat)].fetch_add(amount, std::memory_order_relaxed);
    }
}

inline void Report() {
    if (!Nx2Profile::Enabled()) {
        return;
    }
    const u64 now = Nx2Profile::Now();
    if (g_stats_start == 0) {
        g_stats_start = now;
        return;
    }
    const u64 window = now - g_stats_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    std::array<u64, static_cast<size_t>(Stat::Count)> stats{};
    for (size_t i = 0; i < stats.size(); ++i) {
        stats[i] = g_stats[i].exchange(0, std::memory_order_relaxed);
    }
    const auto rate = [&](Stat stat) {
        return static_cast<double>(stats[static_cast<size_t>(stat)]) * 1e9 /
               static_cast<double>(window);
    };
    LOG_INFO(
        HW_GPU,
        "Buffer cache per second: UBO stream {:.0f}, classic {:.0f} ({:.0f} uploading) | "
        "bank uploads {:.0f} ({:.0f} KiB), skipped {:.0f} | sync uploads {:.0f} ({:.0f} KiB) "
        "| storage resolve contiguous {:.0f}, queried {:.0f} | GPU writes {:.0f} ({:.0f} "
        "already marked) | inline writes direct {:.0f}, walked {:.0f} | hot pages "
        "promoted {:.0f}, demoted {:.0f} ({:.0f} by cost), unchanged {:.0f}, uploaded {:.0f} | "
        "sync skipped {:.0f} ({:.0f} comparing hot pages), walked {:.0f} | storage memo "
        "hits {:.0f} | UBO rebinds kept {:.0f}",
        rate(Stat::UboStream), rate(Stat::UboClassic), rate(Stat::UboClassicUpload),
        rate(Stat::BankUpload), rate(Stat::BankUploadBytes) / 1024.0, rate(Stat::BankSkip),
        rate(Stat::SyncUpload), rate(Stat::SyncUploadBytes) / 1024.0, rate(Stat::ResolveContig),
        rate(Stat::ResolveQuery), rate(Stat::MarkWritten), rate(Stat::MarkAddSkipped),
        rate(Stat::InlineFast), rate(Stat::InlineSlow), rate(Stat::HotPromote),
        rate(Stat::HotDemote), rate(Stat::HotCostDemote), rate(Stat::HotSame),
        rate(Stat::HotUpload), rate(Stat::SyncSkipHit), rate(Stat::SyncSkipHotHit),
        rate(Stat::SyncWalk), rate(Stat::StorageMemoHit), rate(Stat::UboRebindKept));
    const auto protects = Nx2TrackerStats::Take();
    LOG_INFO(HW_GPU,
             "Buffer cache per second: sync precheck clean {:.0f} | UBO stream: slot hits {:.0f}, "
             "range hits {:.0f}, recopied {:.0f}, copied {:.0f} ({:.0f} KiB), over 4 KiB {:.0f} "
             "({:.0f} KiB) | tracker protects {:.0f} ({:.0f} pages), unprotects {:.0f} ({:.0f} "
             "pages) | WriteMemory {:.0f}, on dirty pages only {:.0f}",
             rate(Stat::SyncPrecheckClean), rate(Stat::UboSlotHit), rate(Stat::UboRangeHit),
             rate(Stat::UboRecopied), rate(Stat::UboCopied), rate(Stat::UboCopiedBytes) / 1024.0,
             rate(Stat::UboLarge), rate(Stat::UboLargeBytes) / 1024.0,
             static_cast<double>(protects[0]) * 1e9 / static_cast<double>(window),
             static_cast<double>(protects[1]) * 1e9 / static_cast<double>(window),
             static_cast<double>(protects[2]) * 1e9 / static_cast<double>(window),
             static_cast<double>(protects[3]) * 1e9 / static_cast<double>(window),
             rate(Stat::WriteMemoryCalls), rate(Stat::DirtyWriteSkip));
    LOG_INFO(HW_GPU,
             "Buffer cache banks per second: classic binds of banks {:.0f}, within the stream "
             "size {:.0f} | bank compares {:.0f}, pages compared {:.0f}, already compared this "
             "frame {:.0f} | shadows dropped {:.0f} | pages refreshed by syncs {:.0f}",
             rate(Stat::UboClassicBank), rate(Stat::UboClassicSmall), rate(Stat::BankCompare),
             rate(Stat::BankComparePages), rate(Stat::BankPageRepeat), rate(Stat::BankShadowDrop),
             rate(Stat::BankSyncRefresh));
    g_stats_start = now;
}

} // namespace VideoCommon::Nx2BufCache
