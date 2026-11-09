// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/thread_worker.h"
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

TEST_CASE("Heavy builds waiting for a slot leave the shared workers free", "[video_core][vulkan]") {
    Vulkan::HeavyBuildSlots slots{2};
    Common::ThreadWorker shared{1, "SharedTest"};
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> running{};
    std::atomic<int> most{};
    std::atomic<int> finished{};
    Common::ThreadWorker* const heavy = slots.Workers();
    REQUIRE(heavy != nullptr);
    REQUIRE(heavy != &shared);
    for (int i = 0; i < 5; ++i) {
        heavy->QueueWork([&] {
            const Vulkan::HeavyBuildSlots::Guard guard{slots};
            const int now = ++running;
            int seen = most.load();
            while (now > seen && !most.compare_exchange_weak(seen, now)) {
            }
            gate.wait();
            --running;
            ++finished;
        });
    }
    // Two heavy builds hold the slots and three wait; a light build still runs at once.
    std::atomic<bool> light{};
    shared.QueueWork([&] { light = true; });
    auto light_done = std::async(std::launch::async, [&] { shared.WaitForRequests(); });
    const bool light_ran_first =
        light_done.wait_for(std::chrono::seconds{10}) == std::future_status::ready;
    const int heavy_finished_before = finished.load();
    release.set_value();
    slots.WaitForBuilds();
    light_done.get();
    REQUIRE(light_ran_first);
    REQUIRE(light);
    REQUIRE(heavy_finished_before == 0);
    REQUIRE(finished == 5);
    REQUIRE(most <= 2);
}

TEST_CASE("A build run by its caller counts against the heavy threads' slots",
          "[video_core][vulkan]") {
    Vulkan::HeavyBuildSlots slots{1};
    std::atomic<bool> pool_built{};
    std::optional<Vulkan::HeavyBuildSlots::Guard> caller{std::in_place, slots};
    slots.Workers()->QueueWork([&] {
        const Vulkan::HeavyBuildSlots::Guard guard{slots};
        pool_built = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const bool waited = !pool_built;
    caller.reset();
    slots.WaitForBuilds();
    REQUIRE(waited);
    REQUIRE(pool_built);
}

TEST_CASE("No heavy build threads without a limit", "[video_core][vulkan]") {
    Vulkan::HeavyBuildSlots slots{0};
    REQUIRE(slots.Workers() == nullptr);
    slots.WaitForBuilds(); // nothing started: returns at once
}
