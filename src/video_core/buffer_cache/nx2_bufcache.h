// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon::Nx2BufCache {

[[nodiscard]] inline bool EnvOn(const char* name) {
    const char* const value = std::getenv(name);
    return value == nullptr || value[0] != '0';
}

/// EDEN_NX2_BUFCACHE_CONTIG=0: storage bindings always ask the virtual range cache for their
/// segments, even when the mapping runs show the range is one device range.
inline const bool g_contig_resolve = EnvOn("EDEN_NX2_BUFCACHE_CONTIG");

/// EDEN_NX2_BUFCACHE_BANK=0: a native program's 64 KiB uniform bank is re-uploaded on its first
/// bind of the frame even when that bind streams the buffer from guest memory.
inline const bool g_bank_skip = EnvOn("EDEN_NX2_BUFCACHE_BANK");

/// EDEN_NX2_BUFCACHE_BANKSHADOW=0: that once-a-frame upload copies the whole bank, unchanged
/// pages included (every such copy of a bank the frame already used ends the render pass).
inline const bool g_bank_shadow = EnvOn("EDEN_NX2_BUFCACHE_BANKSHADOW");

/// EDEN_NX2_INLINE_FAST=0: inline writes within one 4 KiB page always walk the GPU page table.
inline const bool g_inline_fast = EnvOn("EDEN_NX2_INLINE_FAST");

/// EDEN_NX2_BUFCACHE_POOLS=0: a storage pointer missing the binding cache always looks its
/// mapping up, even inside a pool (kind range and device run) a previous pointer looked up.
inline const bool g_pool_ranges = EnvOn("EDEN_NX2_BUFCACHE_POOLS");

/// EDEN_NX2_BUFCACHE_GEOMEMO=0: vertex and index buffers are looked up again whenever their
/// registers are dirty, even when they hold the same range.
inline const bool g_geometry_memo = EnvOn("EDEN_NX2_BUFCACHE_GEOMEMO");

/// EDEN_NX2_HOT_PAGES=0: pages the guest rewrites between most syncs are protected again after
/// each upload (a VirtualProtect per sync and a guest fault per write) instead of being kept
/// unprotected and compared at each sync.
inline const bool g_hot_pages = EnvOn("EDEN_NX2_HOT_PAGES");

/// EDEN_NX2_COMPUTE_BINDCACHE=0: compute dispatches look their constant buffers up and copy
/// them to the stream buffer every time, even when unchanged since the previous dispatch.
inline const bool g_compute_bindcache = EnvOn("EDEN_NX2_COMPUTE_BINDCACHE");

/// EDEN_NX2_I2M_COALESCE=0: every inline write takes the buffer cache lock to check its page
/// for GPU-written data, even right after a write that found the same page clean.
inline const bool g_i2m_coalesce = EnvOn("EDEN_NX2_I2M_COALESCE");

/// EDEN_NX2_SYNC_SKIP=0: every sync walks the tracker over its range, even one left clean by
/// an earlier sync with no CPU write marked in the buffer since.
inline const bool g_sync_skip = EnvOn("EDEN_NX2_SYNC_SKIP");

/// EDEN_NX2_BUFCACHE_MARK=0: GPU writes re-add ranges that are already GPU-modified.
inline const bool g_mark_guards = EnvOn("EDEN_NX2_BUFCACHE_MARK");

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
    Count,
};

inline std::array<std::atomic<u64>, static_cast<size_t>(Stat::Count)> g_stats{};
inline u64 g_stats_start{};

inline void Note(Stat stat, u64 amount = 1) {
    if (Nx2Profile::Enabled()) [[unlikely]] {
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
        return static_cast<double>(stats[static_cast<size_t>(stat)]) * 1e9 / window;
    };
    LOG_INFO(HW_GPU,
             "Buffer cache per second: UBO stream {:.0f}, classic {:.0f} ({:.0f} uploading) | "
             "bank uploads {:.0f} ({:.0f} KiB), skipped {:.0f} | sync uploads {:.0f} ({:.0f} KiB) "
             "| storage resolve contiguous {:.0f}, queried {:.0f} | GPU writes {:.0f} ({:.0f} "
             "already marked) | inline writes direct {:.0f}, walked {:.0f} | hot pages "
             "promoted {:.0f}, demoted {:.0f}, unchanged {:.0f}, uploaded {:.0f}",
             rate(Stat::UboStream), rate(Stat::UboClassic), rate(Stat::UboClassicUpload),
             rate(Stat::BankUpload),
             rate(Stat::BankUploadBytes) / 1024.0, rate(Stat::BankSkip), rate(Stat::SyncUpload),
             rate(Stat::SyncUploadBytes) / 1024.0, rate(Stat::ResolveContig),
             rate(Stat::ResolveQuery), rate(Stat::MarkWritten), rate(Stat::MarkAddSkipped),
             rate(Stat::InlineFast), rate(Stat::InlineSlow), rate(Stat::HotPromote),
             rate(Stat::HotDemote), rate(Stat::HotSame), rate(Stat::HotUpload));
    g_stats_start = now;
}

} // namespace VideoCommon::Nx2BufCache
