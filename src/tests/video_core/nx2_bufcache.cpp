// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.h"
#include "common/range_sets.inc"
#include "video_core/buffer_cache/nx2_bufcache.h"

namespace Nx2BufCache = VideoCommon::Nx2BufCache;

TEST_CASE("Page compare finds every differing byte", "[video_core][nx2_bufcache]") {
    std::vector<u8> a(4096 + 37);
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = static_cast<u8>(i * 7 + 3);
    }
    for (const size_t size :
         {size_t{0}, size_t{1}, size_t{127}, size_t{128}, size_t{129}, size_t{4096}, a.size()}) {
        std::vector<u8> b(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(size));
        REQUIRE(Nx2BufCache::BytesEqual(a.data(), b.data(), size));
        for (size_t at = 0; at < size; at += (size > 300 ? 61 : 1)) {
            b[at] ^= 0x80;
            REQUIRE_FALSE(Nx2BufCache::BytesEqual(a.data(), b.data(), size));
            b[at] ^= 0x80;
        }
        if (size != 0) {
            b[size - 1] ^= 1;
            REQUIRE_FALSE(Nx2BufCache::BytesEqual(a.data(), b.data(), size));
        }
    }
}

TEST_CASE("A range is covered only by one range of the set", "[video_core][nx2_bufcache]") {
    Common::RangeSet<u64> set;
    set.Add(0x1000, 0x1000);
    REQUIRE(Nx2BufCache::RangeCovered(set, u64{0x1000}, 0x1000));
    REQUIRE_FALSE(Nx2BufCache::RangeCovered(set, u64{0x1000}, 0x1001));
    REQUIRE_FALSE(Nx2BufCache::RangeCovered(set, u64{0x1000}, 0));
    set.Add(0x2000, 0x1000); // touching ranges join
    REQUIRE(Nx2BufCache::RangeCovered(set, u64{0x1000}, 0x2000));
    set.Add(0x4000, 0x1000);
    REQUIRE_FALSE(Nx2BufCache::RangeCovered(set, u64{0x1000}, 0x4000));
}

TEST_CASE("Frame address sets forget everything at clear", "[video_core][nx2_bufcache]") {
    Nx2BufCache::FrameAddressSet set;
    REQUIRE_FALSE(set.contains(0x10));
    for (u64 key = 1; key <= 1000; ++key) {
        REQUIRE(set.insert(key * 0x1000));
    }
    for (u64 key = 1; key <= 1000; ++key) {
        REQUIRE(set.contains(key * 0x1000));
        REQUIRE_FALSE(set.insert(key * 0x1000));
    }
    REQUIRE_FALSE(set.contains(0x1001));
    set.clear();
    REQUIRE_FALSE(set.contains(0x1000));
    REQUIRE(set.insert(0x1000));
}
