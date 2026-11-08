// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "common/common_types.h"

// The guest CPU's side of the GPU thread's draw input watch (video_core/draw_input_watch.h).
// The GPU thread write-protects the device pages a draw reads (constant buffer words, texture
// pool entries, storage buffer addresses) and arms their bucket here; every guest write to a
// protected page reaches Memory::HandleRasterizerWrite, which calls NoteWrite before anything
// else. A write to a page of an armed bucket moves the bucket's epoch, so the GPU thread sees
// that the page may have changed. Buckets are shared by pages that hash alike: a write to any of
// them moves the epoch (the watch then re-reads more than it must, never less).
namespace Common::InputWatch {

constexpr u32 PageBits = 12;
constexpr size_t NumBuckets = size_t{1} << 16;

struct Table {
    std::array<std::atomic<u32>, NumBuckets> armed{}; ///< armed pages per bucket
    std::array<std::atomic<u32>, NumBuckets> epoch{}; ///< moves with each write to the bucket
    std::atomic<u64> writes{};                        ///< guest writes to armed buckets
    std::atomic<u64> handler_ns{};                    ///< time those writes spent in the handler
};

[[nodiscard]] inline Table& GetTable() {
    static Table table;
    return table;
}

[[nodiscard]] constexpr size_t Bucket(u64 page) {
    return static_cast<size_t>((page * 0x9E3779B97F4A7C15ULL) >> 48) & (NumBuckets - 1);
}

/// A guest write of [address, address + size) to rasterizer-cached memory. Returns whether it
/// hit an armed bucket.
inline bool NoteWrite(u64 address, u64 size) {
    if (size == 0) {
        return false;
    }
    Table& table = GetTable();
    bool hit = false;
    const u64 last = (address + size - 1) >> PageBits;
    for (u64 page = address >> PageBits; page <= last; ++page) {
        const size_t bucket = Bucket(page);
        if (table.armed[bucket].load(std::memory_order_acquire) != 0) {
            table.epoch[bucket].fetch_add(1, std::memory_order_acq_rel);
            hit = true;
        }
    }
    if (hit) {
        table.writes.fetch_add(1, std::memory_order_relaxed);
    }
    return hit;
}

} // namespace Common::InputWatch
