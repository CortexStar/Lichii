// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <random>
#include <set>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/pool_page_watch.h"

using VideoCommon::PoolPageWatch;

TEST_CASE("Pool page watch hands back every ref once, with balanced counts",
          "[video_core][pool_page_watch]") {
    PoolPageWatch watch;
    std::map<u64, int> counts; // cached-page count the watch's refs stand for
    std::set<std::pair<u64, std::pair<u32, u32>>> live;
    std::mt19937 random{0x9a6e};
    const auto drop = [&](u64 device_page, PoolPageWatch::Ref ref) {
        REQUIRE(live.erase({device_page, {ref.table, ref.page}}) == 1);
        REQUIRE(--counts[device_page] >= 0);
    };
    for (unsigned i = 0; i < 20000; ++i) {
        const u64 page = random() % 64;
        const PoolPageWatch::Ref ref{static_cast<u32>(random() % 3),
                                     static_cast<u32>(random() % 8)};
        switch (random() % 5) {
        case 0:
        case 1:
            if (live.emplace(page, std::pair{ref.table, ref.page}).second) {
                watch.Add(page, ref);
                ++counts[page];
            }
            break;
        case 2: {
            const u64 last = page + random() % 4;
            watch.TakeRange(page, last, [&](u64 device_page, PoolPageWatch::Ref taken) {
                REQUIRE(device_page >= page);
                REQUIRE(device_page <= last);
                drop(device_page, taken);
            });
            for (const auto& entry : live) {
                REQUIRE((entry.first < page || entry.first > last));
            }
            break;
        }
        case 3: {
            const bool present = live.contains({page, {ref.table, ref.page}});
            REQUIRE(watch.Remove(page, ref) == present);
            if (present) {
                live.erase({page, {ref.table, ref.page}});
                --counts[page];
            }
            break;
        }
        default:
            if (random() % 50 == 0) {
                const u32 table = ref.table;
                watch.TakeIf([&](u64 device_page) { return device_page % 3 == table; }, drop);
            }
        }
        REQUIRE(watch.NumRefs() == live.size());
    }
    // A range wider than the map takes the other path.
    watch.TakeRange(0, ~u64{0} - 1, drop);
    CHECK(watch.Empty());
    CHECK(live.empty());
    for (const auto& [page, count] : counts) {
        CHECK(count == 0);
    }
}

TEST_CASE("Pool page watch lets the callback add refs again", "[video_core][pool_page_watch]") {
    PoolPageWatch watch;
    watch.Add(5, {0, 1});
    watch.Add(5, {1, 1});
    watch.Add(6, {0, 2});
    unsigned calls{};
    watch.TakeRange(5, 5, [&](u64 device_page, PoolPageWatch::Ref ref) {
        ++calls;
        watch.Add(device_page + 100, ref);
    });
    CHECK(calls == 2);
    CHECK(watch.NumRefs() == 3);
    CHECK(watch.NumPages() == 2);
    CHECK_FALSE(watch.Remove(5, {0, 1}));
    CHECK(watch.Remove(105, {0, 1}));
}
