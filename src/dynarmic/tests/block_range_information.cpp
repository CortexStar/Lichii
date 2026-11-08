// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/backend/block_range_information.h"

#include <boost/icl/interval_set.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "dynarmic/ir/location_descriptor.h"

using namespace Dynarmic;

namespace {
template<typename P>
void Add(Backend::BlockRangeInformation<P>& info, P first, P last, u64 location) {
    info.AddRange(boost::icl::discrete_interval<P>::closed(first, last), IR::LocationDescriptor{location});
}

template<typename P>
::Common::unordered_set<IR::LocationDescriptor> Invalidate(Backend::BlockRangeInformation<P>& info, P first, P last) {
    boost::icl::interval_set<P> set;
    set.add(boost::icl::discrete_interval<P>::closed(first, last));
    return info.InvalidateRanges(set);
}

bool Has(const ::Common::unordered_set<IR::LocationDescriptor>& set, u64 location) {
    return set.contains(IR::LocationDescriptor{location});
}
}  // namespace

TEST_CASE("BlockRangeInformation: only blocks overlapping the range are invalidated", "[backend]") {
    Backend::BlockRangeInformation<u64> info;
    Add<u64>(info, 0x1000, 0x10ff, 1);  // page 1
    Add<u64>(info, 0x1100, 0x11ff, 2);  // page 1, right after block 1
    Add<u64>(info, 0x2000, 0x20ff, 3);  // page 2
    Add<u64>(info, 0x5000, 0x50ff, 4);  // page 5

    // One byte before block 2: block 1 only.
    auto hit = Invalidate<u64>(info, 0x10ff, 0x10ff);
    CHECK(hit.size() == 1);
    CHECK(Has(hit, 1));

    // Block 1 is gone: the same range now hits nothing.
    CHECK(Invalidate<u64>(info, 0x1000, 0x10ff).empty());

    // A gap between blocks, on a page with blocks and on one without.
    CHECK(Invalidate<u64>(info, 0x1200, 0x1fff).empty());
    CHECK(Invalidate<u64>(info, 0x3000, 0x4fff).empty());

    // A range across pages 1 to 2 hits blocks 2 and 3, not block 4.
    hit = Invalidate<u64>(info, 0x11f0, 0x2010);
    CHECK(hit.size() == 2);
    CHECK(Has(hit, 2));
    CHECK(Has(hit, 3));

    hit = Invalidate<u64>(info, 0x5050, 0x5050);
    CHECK(hit.size() == 1);
    CHECK(Has(hit, 4));
}

TEST_CASE("BlockRangeInformation: a block spanning two pages is found from either page and dropped from both", "[backend]") {
    for (const bool hit_second_page : {false, true}) {
        Backend::BlockRangeInformation<u64> info;
        Add<u64>(info, 0x1ff0, 0x200f, 7);  // pages 1 and 2
        Add<u64>(info, 0x1f00, 0x1f0f, 8);  // page 1
        Add<u64>(info, 0x2100, 0x210f, 9);  // page 2

        const u64 at = hit_second_page ? 0x2008 : 0x1ff8;
        auto hit = Invalidate<u64>(info, at, at + 3);
        CHECK(hit.size() == 1);
        CHECK(Has(hit, 7));

        // Gone from both pages, and its neighbours stay.
        CHECK(Invalidate<u64>(info, 0x1ff0, 0x200f).empty());
        hit = Invalidate<u64>(info, 0x1000, 0x2fff);
        CHECK(hit.size() == 2);
        CHECK(Has(hit, 8));
        CHECK(Has(hit, 9));
    }
}

TEST_CASE("BlockRangeInformation: a range larger than the indexed pages invalidates what it covers", "[backend]") {
    Backend::BlockRangeInformation<u64> info;
    Add<u64>(info, 0x1000, 0x1003, 1);
    Add<u64>(info, 0x7fff'fff0, 0x8000'000f, 2);
    Add<u64>(info, 0xffff'ffff'ffff'f000, 0xffff'ffff'ffff'ffff, 3);

    // More pages than buckets: the bucket walk still checks each range against the interval.
    auto hit = Invalidate<u64>(info, 0x2000, 0xffff'ffff'ffff'efff);
    CHECK(hit.size() == 1);
    CHECK(Has(hit, 2));

    hit = Invalidate<u64>(info, 0, ~u64(0));
    CHECK(hit.size() == 2);
    CHECK(Has(hit, 1));
    CHECK(Has(hit, 3));
}

TEST_CASE("BlockRangeInformation: several intervals, duplicates and ClearCache", "[backend]") {
    Backend::BlockRangeInformation<u32> info;
    Add<u32>(info, 0x0ffc, 0x1003, 1);  // pages 0 and 1
    Add<u32>(info, 0x3000, 0x3003, 2);
    Add<u32>(info, 0xffff'fffc, 0xffff'ffff, 3);
    // An empty range is not recorded.
    info.AddRange(boost::icl::discrete_interval<u32>::right_open(0x4000, 0x4000), IR::LocationDescriptor{4});

    boost::icl::interval_set<u32> set;
    set.add(boost::icl::discrete_interval<u32>::closed(0x0ffc, 0x0ffc));
    set.add(boost::icl::discrete_interval<u32>::closed(0x1000, 0x1000));  // block 1 again, other page
    set.add(boost::icl::discrete_interval<u32>::closed(0x4000, 0x4000));
    set.add(boost::icl::discrete_interval<u32>::closed(0xffff'ffff, 0xffff'ffff));
    auto hit = info.InvalidateRanges(set);
    CHECK(hit.size() == 2);
    CHECK(Has(hit, 1));
    CHECK(Has(hit, 3));

    info.ClearCache();
    CHECK(Invalidate<u32>(info, 0, 0xffff'ffff).empty());
}
