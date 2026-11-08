// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <vector>

#include <boost/icl/interval_set.hpp>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename P>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location);
    void ClearCache();
    ::Common::unordered_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<P>& invalidated);

private:
    /// The guest code [first, last] a block was translated from, filed under every 4 KiB page it
    /// touches (blocks are page-sized and page-aligned, so almost always one). Guests such as
    /// Tears of the Kingdom invalidate one 64-byte cache line at a time and often, so an
    /// invalidation has to look at the pages it covers only, not every block compiled: a list
    /// scanned in full cost more per call the longer the game ran. Entries stay 24 bytes, unlike
    /// the ~300 bytes a block of the old interval map of block sets.
    struct Range {
        P first;
        P last;
        IR::LocationDescriptor location;
    };
    static constexpr unsigned page_bits = 12;
    ::Common::unordered_map<P, std::vector<Range>> pages;
};

}  // namespace Dynarmic::Backend
