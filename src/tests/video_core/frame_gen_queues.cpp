// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/frame_gen_queues.h"

namespace {
constexpr VkQueueFlags GRAPHICS = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT |
                                  VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
constexpr VkQueueFlags COMPUTE = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT |
                                 VK_QUEUE_SPARSE_BINDING_BIT;
} // Anonymous namespace

TEST_CASE("AMD FG swapchain queues on an RX 9070 XT's families", "[video_core][amd]") {
    // notes/vulkaninfo-9070xt-26.10.2.txt: graphics x8 and compute x8 (both present), transfer,
    // video encode, video decode.
    const std::array families{
        Vulkan::QueueFamilyTraits{GRAPHICS, 8, true},
        Vulkan::QueueFamilyTraits{COMPUTE, 8, true},
        Vulkan::QueueFamilyTraits{VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT, 1, false},
        Vulkan::QueueFamilyTraits{VK_QUEUE_VIDEO_ENCODE_BIT_KHR, 1, false},
        Vulkan::QueueFamilyTraits{VK_QUEUE_VIDEO_DECODE_BIT_KHR, 1, false},
    };
    const auto plan = Vulkan::PlanFrameGenQueues(families, 0, 0);
    REQUIRE(plan);
    // As AMD's Cauldron sample picks them: a second graphics queue presents, the first family
    // without graphics/compute/transfer acquires, a compute-only queue runs the async work.
    CHECK(plan->present.family == 0);
    CHECK(plan->present.index == 1);
    CHECK(plan->image_acquire.family == 3);
    CHECK(plan->image_acquire.index == 0);
    CHECK(plan->async_compute.family == 1);
    CHECK(plan->async_compute.index == 0);
    CHECK(plan->counts == std::vector<u32>{2, 1, 0, 1, 0});
}

TEST_CASE("AMD FG swapchain queues need free queues", "[video_core][amd]") {
    const std::array one{Vulkan::QueueFamilyTraits{GRAPHICS, 1, true}};
    CHECK(!Vulkan::PlanFrameGenQueues(one, 0, 0));
    const std::array two{Vulkan::QueueFamilyTraits{GRAPHICS, 2, true},
                         Vulkan::QueueFamilyTraits{COMPUTE, 2, false}};
    const auto plan = Vulkan::PlanFrameGenQueues(two, 0, 0);
    REQUIRE(plan);
    CHECK(plan->present.family == 0);
    // No family without graphics/compute/transfer: any non-graphics family acquires.
    CHECK(plan->image_acquire.family == 1);
    CHECK(plan->async_compute.family == 1);
    CHECK(plan->async_compute.index == 1);
    CHECK(!Vulkan::PlanFrameGenQueues(two, 5, 0));
}

TEST_CASE("The present thread's queue follows the FG plan in the graphics family",
          "[video_core][amd]") {
    const std::array families{
        Vulkan::QueueFamilyTraits{GRAPHICS, 8, true},
        Vulkan::QueueFamilyTraits{COMPUTE, 8, true},
        Vulkan::QueueFamilyTraits{VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT, 1, false},
        Vulkan::QueueFamilyTraits{VK_QUEUE_VIDEO_ENCODE_BIT_KHR, 1, false},
        Vulkan::QueueFamilyTraits{VK_QUEUE_VIDEO_DECODE_BIT_KHR, 1, false},
    };
    // RX 9070 XT with AMD's runtime: game 0.0, FG present 0.1, so the present thread takes 0.2.
    const auto plan = Vulkan::PlanFrameGenQueues(families, 0, 0);
    REQUIRE(plan);
    CHECK(Vulkan::PresentThreadQueueIndex(plan, 0, 8) == 2U);
    // Without AMD's runtime: the queue after the game's.
    CHECK(Vulkan::PresentThreadQueueIndex(std::nullopt, 0, 8) == 1U);
    // A family with a single queue has none to spare.
    CHECK(!Vulkan::PresentThreadQueueIndex(std::nullopt, 0, 1));
    // The plan used the family's last queue.
    const std::array two{Vulkan::QueueFamilyTraits{GRAPHICS, 2, true},
                         Vulkan::QueueFamilyTraits{COMPUTE, 2, false}};
    CHECK(!Vulkan::PresentThreadQueueIndex(Vulkan::PlanFrameGenQueues(two, 0, 0), 0, 2));
}
