// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace Vulkan {

/// NVIDIA's estimated driver memory for one built graphics pipeline.
constexpr u64 GraphicsPipelineMemoryEstimate = 450'000;

/// A Switch 2 pipeline-count target of one sixteenth of host RAM.
constexpr size_t Switch2PipelineCountTarget(u64 total_physical_memory) {
    return static_cast<size_t>(total_physical_memory / 16 / GraphicsPipelineMemoryEstimate);
}

/// Returns how many eligible stale pipelines can be removed to approach the target count.
constexpr size_t GraphicsPipelinesToEvict(size_t cached, size_t stale, size_t target) {
    return cached <= target ? 0 : std::min(stale, cached - target);
}

struct EvictionCandidate {
    u64 last_use;
    /// False for a pipeline prebuilt from the disk cache that no draw has used yet.
    bool used;
};

class OldestPipelineSelection {
public:
    OldestPipelineSelection(std::span<u64> last_uses, size_t count) {
        count = std::min(count, last_uses.size());
        if (count == 0) {
            return;
        }
        std::ranges::nth_element(last_uses, last_uses.begin() + (count - 1));
        cutoff = last_uses[count - 1];
        const size_t older =
            std::ranges::count_if(last_uses, [this](u64 last_use) { return last_use < cutoff; });
        equal_remaining = count - older;
    }

    bool Take(u64 last_use) {
        if (last_use < cutoff) {
            return true;
        }
        if (last_use == cutoff && equal_remaining != 0) {
            --equal_remaining;
            return true;
        }
        return false;
    }

private:
    u64 cutoff{};
    size_t equal_remaining{};
};

/// Protects up to `max_protected` unused prebuilt candidates, then selects the oldest `count`.
/// Pass only candidates already old enough, built, and not current. The caller retains evicted
/// pipelines until the scheduler is free.
inline std::vector<size_t> SelectPipelinesToEvict(std::span<const EvictionCandidate> candidates,
                                                  size_t max_protected, size_t count) {
    std::vector<size_t> unprotected;
    std::vector<u64> last_uses;
    unprotected.reserve(candidates.size());
    last_uses.reserve(candidates.size());
    size_t protected_count = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!candidates[i].used && protected_count < max_protected) {
            ++protected_count;
            continue;
        }
        unprotected.push_back(i);
        last_uses.push_back(candidates[i].last_use);
    }
    if (count >= unprotected.size()) {
        return unprotected;
    }
    OldestPipelineSelection oldest{last_uses, count};
    std::vector<size_t> selected;
    selected.reserve(count);
    for (size_t i = 0; i < unprotected.size(); ++i) {
        if (oldest.Take(candidates[unprotected[i]].last_use)) {
            selected.push_back(unprotected[i]);
        }
    }
    return selected;
}

} // namespace Vulkan
