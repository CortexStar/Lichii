// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/heavy_build_slots.h"

TEST_CASE("Heavy build slots follow a quarter of the host RAM, 8 GiB each",
          "[video_core][vulkan]") {
    using Vulkan::HeavyBuildSlots;
    constexpr u64 GiB = u64{1} << 30;
    STATIC_REQUIRE(HeavyBuildSlots::DefaultLimit(16 * GiB) == 1);
    STATIC_REQUIRE(HeavyBuildSlots::DefaultLimit(64 * GiB) == 2);
    STATIC_REQUIRE(HeavyBuildSlots::DefaultLimit(251 * GiB) == 7);
    STATIC_REQUIRE(HeavyBuildSlots::DefaultLimit(1024 * GiB) == 8);
    STATIC_REQUIRE(HeavyBuildSlots::DefaultLimit(0) == 1);
}

TEST_CASE("Heavy builds never run more at once than the slots", "[video_core][vulkan]") {
    Vulkan::HeavyBuildSlots slots{2};
    std::atomic<int> running{};
    std::atomic<int> most{};
    std::atomic<int> finished{};
    std::vector<std::jthread> threads;
    for (int i = 0; i < 6; ++i) {
        threads.emplace_back([&] {
            const Vulkan::HeavyBuildSlots::Guard guard{slots};
            const int now = ++running;
            int seen = most.load();
            while (now > seen && !most.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            --running;
            ++finished;
        });
    }
    threads.clear();
    REQUIRE(finished == 6);
    // Without the limit all six overlap in their 20 ms (most == 6).
    REQUIRE(most <= 2);
}

TEST_CASE("Zero heavy build slots means no limit", "[video_core][vulkan]") {
    Vulkan::HeavyBuildSlots slots{0};
    const Vulkan::HeavyBuildSlots::Guard first{slots};
    const Vulkan::HeavyBuildSlots::Guard second{slots}; // would block with a limit of 1
    REQUIRE(slots.Limit() == 0);
}
