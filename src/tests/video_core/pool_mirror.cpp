// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>
#include <map>
#include <random>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/pool_mirror.h"

namespace PoolMirror = VideoCommon::PoolMirror;

TEST_CASE("Pool mirror layout keeps sampler slots when classes are added", "[pool_mirror]") {
    PoolMirror::Layout layout{.tic_stride = 1000, .tsc_stride = 300, .num_classes = 2};
    CHECK(layout.ImageSlot(0, 5) == 5);
    CHECK(layout.ImageSlot(2, 5) == 2005);
    CHECK(layout.ImageSlots() == 9000);
    const u32 slot = layout.SamplerSlot(1, true, 7);
    CHECK(slot == 3 * 300 + 7);
    layout.num_classes = 3;
    CHECK(layout.SamplerSlot(1, true, 7) == slot);
    CHECK(layout.SamplerSlots() == 1800);
    CHECK(PoolMirror::Layout::ImagePage(127) == 0);
    CHECK(PoolMirror::Layout::ImagePage(128) == 1);
    CHECK(PoolMirror::Layout::SamplerPage(256) == 1);
}

TEST_CASE("Sampler classes intern view keys", "[pool_mirror]") {
    PoolMirror::SamplerClasses classes;
    CHECK(classes.Find(0x10) == 0);
    CHECK(classes.Find(0x20) == 1);
    CHECK(classes.Find(0x10) == 0);
    CHECK(classes.Size() == 2);
    CHECK(classes.Key(1) == 0x20);
}

// A model GPU: command buffers (ticks) hold draws; each draw reads the physical page the table
// maps when it is recorded. A page must never be written while a recorded draw that read it is
// not complete, and must hold what it held when that draw was recorded.
TEST_CASE("Heap page versions never write a page a pending draw read", "[pool_mirror]") {
    PoolMirror::HeapPageVersions versions;
    std::mt19937 random{0xc0de};
    u64 draw_seq = 0;
    u64 tick = 1;
    u64 completed = 0;                 // ticks <= completed are free
    std::map<u32, u64> contents;       // physical page -> value
    std::map<u32, u64> logical_values; // what a draw must read now
    std::deque<std::pair<u64, std::vector<std::pair<u32, u64>>>> pending; // tick -> reads
    std::vector<std::pair<u32, u64>> current_reads;
    const auto is_free = [&](u64 t) { return t <= completed; };
    for (unsigned i = 0; i < 50000; ++i) {
        const u32 logical = random() % 16;
        switch (random() % 6) {
        case 0:
        case 1: {
            const u64 value = random();
            const auto target = versions.Write(logical, draw_seq, tick, is_free);
            // A fresh page holds whatever its last use left until it is filled.
            if (target.fresh) {
                contents[target.physical] =
                    target.copy_from != PoolMirror::HeapPageVersions::NO_PAGE
                        ? contents[target.copy_from]
                        : 0;
            } else {
                REQUIRE(target.copy_from == PoolMirror::HeapPageVersions::NO_PAGE);
            }
            contents[target.physical] = value;
            logical_values[logical] = value;
            break;
        }
        case 2: {
            // A draw reads every logical page through the table.
            ++draw_seq;
            const auto table = versions.Table();
            for (u32 page = 0; page < table.size(); ++page) {
                if (table[page] == PoolMirror::HeapPageVersions::NO_PAGE) {
                    continue;
                }
                REQUIRE(contents[table[page]] == logical_values[page]);
                current_reads.emplace_back(table[page], contents[table[page]]);
            }
            break;
        }
        case 3:
            // Submit: the command buffer is pending.
            pending.emplace_back(tick, std::move(current_reads));
            current_reads.clear();
            ++tick;
            break;
        case 4:
            // The GPU completes the oldest command buffer: what its draws read is unchanged.
            if (!pending.empty()) {
                for (const auto& [physical, value] : pending.front().second) {
                    REQUIRE(contents[physical] == value);
                }
                completed = pending.front().first;
                pending.pop_front();
            }
            break;
        default:
            if (random() % 200 == 0) {
                versions.Reset(tick);
                logical_values.clear();
            }
        }
        // Draws recorded in the open command buffer read pages that must stay unchanged too.
        for (const auto& [physical, value] : current_reads) {
            REQUIRE(contents[physical] == value);
        }
    }
    CHECK(versions.NumPhysicalPages() < 200);
}

TEST_CASE("Heap page versions write in place until a draw is recorded", "[pool_mirror]") {
    PoolMirror::HeapPageVersions versions;
    const auto never = [](u64) { return false; };
    const auto first = versions.Write(3, 0, 1, never);
    CHECK(first.copy_from == PoolMirror::HeapPageVersions::NO_PAGE);
    CHECK(first.fresh);
    CHECK(versions.TakeTableChanged());
    const auto again = versions.Write(3, 0, 1, never);
    CHECK(again.physical == first.physical);
    CHECK(again.copy_from == PoolMirror::HeapPageVersions::NO_PAGE);
    CHECK_FALSE(again.fresh);
    CHECK_FALSE(versions.TakeTableChanged());
    const auto after_draw = versions.Write(3, 1, 1, never);
    CHECK(after_draw.physical != first.physical);
    CHECK(after_draw.copy_from == first.physical);
    CHECK(after_draw.fresh);
    CHECK(versions.TakeTableChanged());
    CHECK(versions.IsRetired(first.physical));
    // Recycled once its tick is free.
    const auto recycled = versions.Write(3, 2, 2, [](u64 t) { return t <= 1; });
    CHECK(recycled.physical == first.physical);
    CHECK(recycled.copy_from == after_draw.physical);
}
