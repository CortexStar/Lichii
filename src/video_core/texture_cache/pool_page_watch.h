// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/container/unordered_map.h"

namespace VideoCommon {

/// The device pages holding descriptor pool entries the texture cache keeps resident, and the
/// table pages watched on each. Every ref stands for one cached-page count the texture cache
/// holds on the device page (DeviceMemoryManager::UpdatePagesCachedCount), so guest writes to it
/// trap and reach TextureCache::WriteMemory. The caller adds the count with Add and drops one
/// for each ref the Take functions hand back.
class PoolPageWatch {
public:
    struct Ref {
        u32 table; ///< Channel * 4 + kind (graphics TIC, graphics TSC, compute TIC, compute TSC)
        u32 page;  ///< Page of that table (DescriptorTable::PageOf)

        bool operator==(const Ref&) const = default;
    };

    void Add(u64 device_page, Ref ref) {
        pages[device_page].push_back(ref);
        ++num_refs;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return num_refs == 0;
    }

    [[nodiscard]] size_t NumRefs() const noexcept {
        return num_refs;
    }

    [[nodiscard]] size_t NumPages() const noexcept {
        return pages.size();
    }

    /// Removes every ref on device pages [first_page, last_page] and then calls
    /// func(device_page, ref) for each (func may add refs).
    template <typename Func>
    void TakeRange(u64 first_page, u64 last_page, Func&& func) {
        if (num_refs == 0 || last_page < first_page) {
            return;
        }
        taken.clear();
        if (last_page - first_page < pages.size()) {
            for (u64 page = first_page; page <= last_page; ++page) {
                if (const auto it = pages.find(page); it != pages.end()) {
                    TakeEntry(it);
                }
            }
        } else {
            CollectIf([&](u64 page) { return page >= first_page && page <= last_page; });
        }
        Hand(func);
    }

    /// Removes every ref on device pages for which pred(device_page) holds, then calls
    /// func(device_page, ref) for each.
    template <typename Pred, typename Func>
    void TakeIf(Pred&& pred, Func&& func) {
        if (num_refs == 0) {
            return;
        }
        taken.clear();
        CollectIf(pred);
        Hand(func);
    }

    /// Removes one ref (a table rebinding its pool). False when it was not there.
    bool Remove(u64 device_page, Ref ref) {
        const auto it = pages.find(device_page);
        if (it == pages.end()) {
            return false;
        }
        auto& refs = it->second;
        const auto ref_it = std::ranges::find(refs, ref);
        if (ref_it == refs.end()) {
            return false;
        }
        refs.erase(ref_it);
        --num_refs;
        if (refs.empty()) {
            pages.erase(it);
        }
        return true;
    }

private:
    using Refs = boost::container::small_vector<Ref, 2>;
    using Map = ::Common::unordered_map<u64, Refs>;

    void TakeEntry(typename Map::iterator it) {
        for (const Ref& ref : it->second) {
            taken.emplace_back(it->first, ref);
        }
        num_refs -= it->second.size();
        pages.erase(it);
    }

    template <typename Pred>
    void CollectIf(Pred&& pred) {
        for (auto it = pages.begin(); it != pages.end();) {
            if (pred(it->first)) {
                for (const Ref& ref : it->second) {
                    taken.emplace_back(it->first, ref);
                }
                num_refs -= it->second.size();
                it = pages.erase(it);
            } else {
                ++it;
            }
        }
    }

    template <typename Func>
    void Hand(Func& func) {
        // A copy: func may add refs and take again.
        const std::vector<std::pair<u64, Ref>> refs = std::move(taken);
        taken.clear();
        for (const auto& [device_page, ref] : refs) {
            func(device_page, ref);
        }
    }

    Map pages;
    std::vector<std::pair<u64, Ref>> taken;
    size_t num_refs{};
};

} // namespace VideoCommon
