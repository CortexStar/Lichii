// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/gpl_build_once.h"

using Map = Vulkan::BuildOnceMap<int>;
using Outcome = Map::Outcome;

TEST_CASE("A library is built once and shared", "[video_core][gpl]") {
    Map map;
    std::atomic<int> builds{};
    const auto build = [&] {
        ++builds;
        return std::make_shared<const int>(7);
    };
    Outcome first{}, second{};
    REQUIRE(*map.Get("a", build, &first) == 7);
    REQUIRE(*map.Get("a", build, &second) == 7);
    CHECK(first == Outcome::Built);
    CHECK(second == Outcome::Hit);
    CHECK(builds == 1);
    CHECK(map.Ready("a"));
    CHECK_FALSE(map.Ready("b"));
}

TEST_CASE("A failed library stays failed instead of being built again", "[video_core][gpl]") {
    Map map;
    std::atomic<int> builds{};
    REQUIRE_THROWS_AS(map.Get("a",
                              [&]() -> std::shared_ptr<const int> {
                                  ++builds;
                                  throw std::runtime_error("driver");
                              }),
                      std::runtime_error);
    CHECK(map.Ready("a"));
    CHECK(map.Get("a", [&] {
        ++builds;
        return std::make_shared<const int>(1);
    }) == nullptr);
    CHECK(builds == 1);
}

TEST_CASE("The pipeline's own build takes over a prefetch no compile thread started",
          "[video_core][gpl]") {
    Map map;
    std::atomic<int> builds{};
    // The pipeline asks first: it builds the library itself.
    REQUIRE(*map.Get("pre", [&] {
        ++builds;
        return std::make_shared<const int>(1);
    }) == 1);
    // The compile thread reaches the queued prefetch afterwards: nothing left to build, and its
    // builder (which may point at a pipeline gone by now) is never called.
    bool called = false;
    CHECK(map.BuildIfAbsent("pre", [&] {
        called = true;
        return std::make_shared<const int>(2);
    }) == Outcome::Hit);
    CHECK_FALSE(called);
    CHECK(builds == 1);
}

TEST_CASE("Both parts of a pipeline build at the same time", "[video_core][gpl]") {
    Map map;
    std::promise<void> prefetch_started;
    std::promise<void> fragment_done;
    auto fragment_future = fragment_done.get_future().share();
    // The compile thread holds the pre-rasterization library until the fragment one is built on
    // the pipeline's thread: with the parts in turn this would never finish.
    auto prefetch = std::async(std::launch::async, [&] {
        return map.BuildIfAbsent("pre", [&] {
            prefetch_started.set_value();
            fragment_future.wait();
            return std::make_shared<const int>(1);
        });
    });
    prefetch_started.get_future().wait();
    Outcome fragment_outcome{}, pre_outcome{};
    const auto fragment =
        map.Get("frag", [] { return std::make_shared<const int>(2); }, &fragment_outcome);
    fragment_done.set_value();
    // The pipeline waits for the prefetched part instead of building it twice.
    const auto pre = map.Get("pre", [] { return std::make_shared<const int>(3); }, &pre_outcome);
    CHECK(prefetch.get() == Outcome::Built);
    CHECK(*fragment == 2);
    CHECK(*pre == 1);
    CHECK(fragment_outcome == Outcome::Built);
    CHECK((pre_outcome == Outcome::Waited || pre_outcome == Outcome::Hit));
}

TEST_CASE("A prefetch leaves a library being built to its builder", "[video_core][gpl]") {
    Map map;
    std::promise<void> started, release;
    auto gate = release.get_future().share();
    auto owner = std::async(std::launch::async, [&] {
        return map.Get("a", [&] {
            started.set_value();
            gate.wait();
            return std::make_shared<const int>(5);
        });
    });
    started.get_future().wait();
    // Returns at once instead of blocking a compile thread on someone else's build.
    auto prefetch = std::async(std::launch::async, [&] {
        return map.BuildIfAbsent("a", [] { return std::make_shared<const int>(6); });
    });
    const bool immediate = prefetch.wait_for(std::chrono::seconds{5}) == std::future_status::ready;
    release.set_value();
    CHECK(immediate);
    CHECK(prefetch.get() == Outcome::Busy);
    CHECK(*owner.get() == 5);
    CHECK(map.Size() == 1);
}
