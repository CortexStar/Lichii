// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

#include <vulkan/vulkan_core.h>

#include "common/common_types.h"

namespace Vulkan {

/// What the planner needs to know of a queue family.
struct QueueFamilyTraits {
    VkQueueFlags flags{};
    u32 count{};
    bool present{}; ///< Can present to the window's surface.
};

struct QueueSlot {
    u32 family{};
    u32 index{};
};

/// The queues AMD's frame generation swapchain takes besides the game's (graphics) queue
/// (ffxCreateContextDescFrameGenerationSwapChainVK): present (transfer + present; AMD's sample
/// takes graphics + compute + present first), async compute (compute-only family), image acquire
/// (any; AMD's sample takes a family without graphics, compute or transfer first, then any
/// family without graphics). Picked in the order of AMD's Cauldron sample (device_vk.cpp
/// GetQueues), from the queues the game's graphics and present queues leave free.
struct FrameGenQueuePlan {
    QueueSlot present;
    QueueSlot async_compute;
    QueueSlot image_acquire;
    /// Queues to create per family (index = family), the game's included.
    std::vector<u32> counts;
};

[[nodiscard]] inline std::optional<FrameGenQueuePlan> PlanFrameGenQueues(
    std::span<const QueueFamilyTraits> families, u32 graphics_family, u32 present_family) {
    std::vector<u32> used(families.size(), 0);
    if (graphics_family >= families.size() || present_family >= families.size()) {
        return std::nullopt;
    }
    used[graphics_family] = 1;
    used[present_family] = (std::max)(used[present_family], 1U);
    const auto has = [&](u32 family, VkQueueFlags flags) {
        return (families[family].flags & flags) == flags;
    };
    const auto take = [&](auto&& accept) -> std::optional<QueueSlot> {
        for (u32 family = 0; family < families.size(); ++family) {
            if (used[family] < families[family].count && accept(family)) {
                return QueueSlot{family, used[family]++};
            }
        }
        return std::nullopt;
    };
    constexpr VkQueueFlags GRAPHICS_COMPUTE = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    auto present = take([&](u32 f) { return has(f, GRAPHICS_COMPUTE) && families[f].present; });
    if (!present) {
        present = take([&](u32 f) { return has(f, VK_QUEUE_TRANSFER_BIT) && families[f].present; });
    }
    auto acquire = take([&](u32 f) {
        return (families[f].flags &
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) == 0;
    });
    if (!acquire) {
        acquire = take([&](u32 f) { return (families[f].flags & VK_QUEUE_GRAPHICS_BIT) == 0; });
    }
    auto compute = take([&](u32 f) {
        return has(f, VK_QUEUE_COMPUTE_BIT) && (families[f].flags & VK_QUEUE_GRAPHICS_BIT) == 0;
    });
    if (!compute) {
        compute = take([&](u32 f) { return has(f, VK_QUEUE_COMPUTE_BIT); });
    }
    if (!present || !acquire || !compute) {
        return std::nullopt;
    }
    return FrameGenQueuePlan{
        .present = *present,
        .async_compute = *compute,
        .image_acquire = *acquire,
        .counts = used,
    };
}

/**
 * Index of the present thread's own queue in the graphics family (PresentManager presents on it,
 * so its copies and flips don't wait behind the game's work): the family's next queue after the
 * game's and, when AMD's frame generation plan exists, after the plan's. The plan exists whenever
 * AMD's runtime loads, frame generation on or off, so the index doesn't move with it.
 *
 * @param plan            PlanFrameGenQueues' result, if it was made.
 * @param graphics_family The game's graphics queue family (it presents too).
 * @param family_count    Queues the graphics family has.
 * @return The index, or none when the family has no queue left.
 */
[[nodiscard]] inline std::optional<u32> PresentThreadQueueIndex(
    const std::optional<FrameGenQueuePlan>& plan, u32 graphics_family, u32 family_count) {
    const u32 index = plan && graphics_family < plan->counts.size()
                          ? (std::max)(plan->counts[graphics_family], 1U)
                          : 1U;
    return index < family_count ? std::optional<u32>{index} : std::nullopt;
}

} // namespace Vulkan
