// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <limits>

namespace Vulkan {

/// Where a graphics pipeline read from the disk cache is built.
enum class CachedPipelinePlacement {
    /// On the boot workers, before the game starts.
    Boot,
    /// On the background prebuild threads once the game runs; a draw that needs it first takes
    /// the runtime route instead.
    Background,
    /// Not built until a draw needs it.
    Skip,
};

struct CachedPipelinePolicy {
    /// EDEN_NX2_PRELOAD_ALL_PIPELINES: every cached pipeline at boot.
    bool preload_all{};
    /// A list of the pipelines recent sessions used exists.
    bool has_used_list{};
    /// Boot builds without a used list (the first boot with this cache).
    size_t first_boot_limit{};
    /// Pipelines the boot and the background prebuild build together at most: the memory budget
    /// of the title's pipelines.
    size_t total_budget{std::numeric_limits<size_t>::max()};
    /// Background prebuilds at most.
    size_t background_limit{};
};

/// Pipelines built so far by the boot and admitted to the background prebuild.
struct CachedPipelineCounts {
    size_t boot{};
    size_t background{};
};

/// The pipelines recent sessions used are built at boot (or the first `first_boot_limit` without
/// a list). The rest are built in the background up to the budget: a pipeline in the disk cache
/// that a draw reaches before it is built costs a translation on the GPU thread and a driver
/// compile the draw waits for, where one built in the background is bound at once. Past the
/// budget, the rest wait for a draw.
[[nodiscard]] constexpr CachedPipelinePlacement PlaceCachedPipeline(
    const CachedPipelinePolicy& policy, bool recently_used, CachedPipelineCounts counts) {
    if (policy.preload_all) {
        return CachedPipelinePlacement::Boot;
    }
    if (policy.has_used_list ? recently_used : counts.boot < policy.first_boot_limit) {
        return CachedPipelinePlacement::Boot;
    }
    if (counts.background < policy.background_limit &&
        counts.boot + counts.background < policy.total_budget) {
        return CachedPipelinePlacement::Background;
    }
    return CachedPipelinePlacement::Skip;
}

/// Background prebuilds of a title whose pipelines are kept up to a memory budget (Switch 2):
/// all within the budget. Other titles evict pipelines unused for a while, so only a few are
/// built ahead.
inline constexpr size_t LegacyBackgroundPrebuildLimit = 64;

[[nodiscard]] constexpr CachedPipelinePolicy MakeCachedPipelinePolicy(
    bool preload_all, bool has_used_list, size_t first_boot_limit, bool budgeted, size_t budget) {
    return {
        .preload_all = preload_all,
        .has_used_list = has_used_list,
        .first_boot_limit = first_boot_limit,
        .total_budget = budgeted ? budget : std::numeric_limits<size_t>::max(),
        .background_limit =
            budgeted ? std::numeric_limits<size_t>::max() : LegacyBackgroundPrebuildLimit,
    };
}

} // namespace Vulkan
