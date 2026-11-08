// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <algorithm>

#include <boost/icl/interval_set.hpp>
#include "common/common_types.h"
#include "common/container/unordered_set.h"

namespace Dynarmic::Backend {

template<typename P>
void BlockRangeInformation<P>::AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location) {
    if (boost::icl::is_empty(range)) {
        return;
    }
    const Range entry{boost::icl::first(range), boost::icl::last(range), location};
    for (u64 page = u64(entry.first) >> page_bits; page <= u64(entry.last) >> page_bits; ++page) {
        pages[P(page)].push_back(entry);
    }
}

template<typename P>
void BlockRangeInformation<P>::ClearCache() {
    pages.clear();
}

template<typename P>
::Common::unordered_set<IR::LocationDescriptor> BlockRangeInformation<P>::InvalidateRanges(const boost::icl::interval_set<P>& invalidated) {
    ::Common::unordered_set<IR::LocationDescriptor> erase_locations;
    std::vector<Range> hits;
    const auto collect = [&](const std::vector<Range>& bucket, P lo, P hi) {
        for (const Range& range : bucket) {
            if (range.first <= hi && lo <= range.last && erase_locations.insert(range.location).second) {
                hits.push_back(range);
            }
        }
    };
    for (const auto& interval : invalidated) {
        const P lo = boost::icl::first(interval);
        const P hi = boost::icl::last(interval);
        const u64 first_page = u64(lo) >> page_bits;
        const u64 last_page = u64(hi) >> page_bits;
        if (last_page - first_page >= pages.size()) {
            // A large unmap: fewer buckets than pages in the interval, so walk the buckets.
            for (const auto& [page, bucket] : pages) {
                collect(bucket, lo, hi);
            }
        } else {
            for (u64 page = first_page; page <= last_page; ++page) {
                if (const auto it = pages.find(P(page)); it != pages.end()) {
                    collect(it->second, lo, hi);
                }
            }
        }
    }
    // Drop each invalidated block from every page it was filed under.
    for (const Range& hit : hits) {
        for (u64 page = u64(hit.first) >> page_bits; page <= u64(hit.last) >> page_bits; ++page) {
            const auto it = pages.find(P(page));
            if (it == pages.end()) {
                continue;
            }
            std::erase_if(it->second, [&](const Range& range) { return range.location == hit.location; });
            if (it->second.empty()) {
                pages.erase(it);
            }
        }
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
