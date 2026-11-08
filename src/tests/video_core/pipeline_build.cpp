// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_build.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_prebuild.h"

namespace {
using Vulkan::EmissionBudget;
using Vulkan::PipelineBuild;

bool Pending(std::future<void>& result) {
    return result.wait_for(std::chrono::seconds{0}) != std::future_status::ready;
}
} // namespace

TEST_CASE("Production pipeline build holds a draw without blocking its producer",
          "[video_core][async_draw][pipeline_build]") {
    Common::ThreadWorker workers{1, "PipelineTest"};
    EmissionBudget budget{1};
    std::promise<void> release, entered;
    auto gate = release.get_future().share();
    int output = 0;
    PipelineBuild build{&workers, [reservation = std::shared_ptr{budget.TryAcquire(true)},
                                   gate, &entered, &output] {
                            entered.set_value();
                            gate.wait();
                            output = 42;
                        }};
    entered.get_future().wait();
    const bool building = !build.IsBuilt();
    const bool saturated = !budget.TryAcquire(true);
    auto draw = std::async(std::launch::async, [&] { build.Wait(); });
    const bool held = Pending(draw);
    int producer_progress = 0;
    ++producer_progress;
    release.set_value();
    draw.get();
    REQUIRE(building);
    REQUIRE(saturated);
    REQUIRE(held);
    REQUIRE(producer_progress == 1);
    REQUIRE(output == 42);
    REQUIRE(build.IsBuilt());
    REQUIRE_FALSE(build.Failed());
    REQUIRE(budget.Pending() == 0);
    REQUIRE(budget.TryAcquire(true));
}

TEST_CASE("Out of order pipeline completions preserve scheduler draw order",
          "[video_core][async_draw][pipeline_build]") {
    Common::ThreadWorker workers{2, "PipelineTest"};
    std::promise<void> release_first, release_second;
    auto first_gate = release_first.get_future().share();
    auto second_gate = release_second.get_future().share();
    PipelineBuild first{&workers, [first_gate] { first_gate.wait(); }};
    PipelineBuild second{&workers, [second_gate] { second_gate.wait(); }};
    std::vector<int> draws;
    auto scheduler = std::async(std::launch::async, [&] {
        first.Wait();
        draws.push_back(1);
        second.Wait();
        draws.push_back(2);
    });
    release_second.set_value();
    second.Wait();
    const bool first_pending = !first.IsBuilt();
    const bool scheduler_held = Pending(scheduler);
    release_first.set_value();
    scheduler.get();
    REQUIRE(first_pending);
    REQUIRE(scheduler_held);
    REQUIRE(draws == std::vector<int>{1, 2});
}

TEST_CASE("Pipeline worker exceptions release captures and notify every draw waiter",
          "[video_core][async_draw][pipeline_build]") {
    for (const bool standard_exception : {false, true}) {
        Common::ThreadWorker workers{1, "PipelineTest"};
        EmissionBudget budget{1};
        std::promise<void> release, entered;
        auto gate = release.get_future().share();
        PipelineBuild build{&workers, [reservation = std::shared_ptr{budget.TryAcquire(true)},
                                       gate, &entered, standard_exception] {
                                entered.set_value();
                                gate.wait();
                                if (standard_exception) {
                                    throw std::runtime_error("emission failed");
                                }
                                throw 42;
                            }};
        entered.get_future().wait();
        auto first = std::async(std::launch::async, [&] { build.Wait(); });
        auto second = std::async(std::launch::async, [&] { build.Wait(); });
        release.set_value();
        first.get();
        second.get();
        REQUIRE(build.IsBuilt());
        REQUIRE(build.Failed());
        REQUIRE(budget.Pending() == 0);
        REQUIRE(budget.TryAcquire(true));
    }
}

TEST_CASE("Eviction after an inline build leaves a safe queued callback",
          "[video_core][async_draw][pipeline_build]") {
    Common::ThreadWorker workers{1, "PipelineTest"};
    std::promise<void> release, entered;
    auto gate = release.get_future().share();
    workers.QueueWork([gate, &entered] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    int builds = 0;
    auto build = std::make_unique<PipelineBuild>(&workers, [&] { ++builds; });
    build->Wait(true);
    const bool evictable = build->IsBuilt();
    build.reset();
    release.set_value();
    workers.WaitForRequests();
    REQUIRE(evictable);
    REQUIRE(builds == 1);
}

TEST_CASE("Pipeline destruction drains queued work before owner resources disappear",
          "[video_core][async_draw][pipeline_build]") {
    Common::ThreadWorker workers{1, "PipelineTest"};
    EmissionBudget budget{1};
    std::promise<void> release, entered;
    auto gate = release.get_future().share();
    workers.QueueWork([gate, &entered] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    auto resource = std::make_unique<int>(0);
    auto build = std::make_unique<PipelineBuild>(
        &workers, [reservation = std::shared_ptr{budget.TryAcquire(true)},
                   output = resource.get()] { *output = 42; });
    build.reset();
    const int result = *resource;
    resource.reset();
    release.set_value();
    workers.WaitForRequests();
    REQUIRE(result == 42);
    REQUIRE(budget.Pending() == 0);
}

TEST_CASE("Pipeline destruction waits for a running callback and its cleanup",
          "[video_core][async_draw][pipeline_build]") {
    Common::ThreadWorker workers{1, "PipelineTest"};
    EmissionBudget budget{1};
    std::promise<void> release, entered;
    auto gate = release.get_future().share();
    auto resource = std::make_unique<int>(0);
    auto build = std::make_unique<PipelineBuild>(
        &workers, [reservation = std::shared_ptr{budget.TryAcquire(true)}, gate, &entered,
                   output = resource.get()] {
            entered.set_value();
            gate.wait();
            *output = 42;
        });
    entered.get_future().wait();
    auto destroy = std::async(std::launch::async, [&] { build.reset(); });
    const bool destruction_held = Pending(destroy);
    release.set_value();
    destroy.get();
    REQUIRE(destruction_held);
    REQUIRE(*resource == 42);
    REQUIRE(budget.Pending() == 0);
}

TEST_CASE("Emission reservations enforce the production fallback and survive budget teardown",
          "[video_core][async_draw][pipeline_build]") {
    auto budget = std::make_unique<EmissionBudget>(2);
    REQUIRE_FALSE(budget->TryAcquire(false));
    auto first = budget->TryAcquire(true);
    auto second = budget->TryAcquire(true);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE_FALSE(budget->TryAcquire(true));
    first.reset();
    REQUIRE(budget->TryAcquire(true));
    REQUIRE(budget->Pending() == 1);
    budget.reset();
    second.reset();
}

TEST_CASE("Cached prebuild refills capture and resident budgets without losing overflow",
          "[video_core][pipeline_prebuild]") {
    Vulkan::PipelinePrebuild<int, int> cache{2, 100, false};
    std::promise<void> entered_first, release_first, entered_second, entered_third;
    auto gate = release_first.get_future().share();
    auto second = entered_second.get_future();
    auto third = entered_third.get_future();
    REQUIRE(cache.Add(1, 60, [&] {
        entered_first.set_value();
        gate.wait();
        return std::make_unique<int>(1);
    }));
    REQUIRE(cache.Add(2, 60, [&] {
        entered_second.set_value();
        return std::make_unique<int>(2);
    }));
    REQUIRE(cache.Add(3, 60, [&] {
        entered_third.set_value();
        return std::make_unique<int>(3);
    }));
    REQUIRE_FALSE(cache.Add(1, 1, [] { return std::make_unique<int>(99); }));
    cache.Start(2);
    entered_first.get_future().wait();
    const bool capture_bounded = Pending(second);
    release_first.set_value();
    second.wait();
    const bool resident_bounded = Pending(third);
    auto first = cache.Take(1);
    third.wait();
    auto second_pipeline = cache.Take(2);
    auto third_pipeline = cache.Take(3);
    cache.Stop();
    REQUIRE(capture_bounded);
    REQUIRE(resident_bounded);
    REQUIRE(first);
    REQUIRE(*first == 1);
    REQUIRE(second_pipeline);
    REQUIRE(*second_pipeline == 2);
    REQUIRE(third_pipeline);
    REQUIRE(*third_pipeline == 3);
}

TEST_CASE("Demand shares running translation and returns before driver completion",
          "[video_core][pipeline_prebuild]") {
    struct PendingPipeline {
        int metadata = 42;
        PipelineBuild driver;
        PendingPipeline(Common::ThreadWorker& workers, std::shared_future<void> gate)
            : driver{&workers, [gate] { gate.wait(); }} {}
    };
    Common::ThreadWorker drivers{1, "DriverTest"};
    Vulkan::PipelinePrebuild<int, PendingPipeline> cache{1, 100, false};
    std::promise<void> entered, release_translation, release_driver;
    auto translation_gate = release_translation.get_future().share();
    auto driver_gate = release_driver.get_future().share();
    std::atomic<int> translations{};
    REQUIRE(cache.Add(1, 1, [&] {
        ++translations;
        entered.set_value();
        translation_gate.wait();
        return std::make_unique<PendingPipeline>(drivers, driver_gate);
    }));
    cache.Start(1);
    entered.get_future().wait();
    auto demand = std::async(std::launch::async, [&] { return cache.Take(1); });
    const bool translation_pending = demand.wait_for(std::chrono::seconds{0}) !=
                                     std::future_status::ready;
    release_translation.set_value();
    const bool metadata_published = demand.wait_for(std::chrono::seconds{2}) ==
                                    std::future_status::ready;
    // Always release the driver even when a regression blocks demand on it.
    bool driver_pending = false;
    std::unique_ptr<PendingPipeline> pipeline;
    if (metadata_published) {
        pipeline = demand.get();
        driver_pending = pipeline && !pipeline->driver.IsBuilt();
    }
    release_driver.set_value();
    if (!metadata_published) {
        pipeline = demand.get();
    }
    cache.Stop();
    REQUIRE(translation_pending);
    REQUIRE(metadata_published);
    REQUIRE(driver_pending);
    REQUIRE(translations == 1);
    REQUIRE(pipeline);
    REQUIRE(pipeline->metadata == 42);
    REQUIRE_FALSE(cache.Take(1));
}

TEST_CASE("Demand steals a queued prebuild exactly once without waiting behind other work",
          "[video_core][pipeline_prebuild]") {
    Vulkan::PipelinePrebuild<int, int> cache{2, 100, false};
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    std::atomic<int> demanded_builds{};
    REQUIRE(cache.Add(1, 1, [&] {
        entered.set_value();
        gate.wait();
        return std::make_unique<int>(1);
    }));
    REQUIRE(cache.Add(2, 1, [&] {
        ++demanded_builds;
        return std::make_unique<int>(2);
    }));
    cache.Start(1);
    entered.get_future().wait();
    auto demand = std::async(std::launch::async, [&] { return cache.Take(2); });
    const bool immediate = demand.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
    release.set_value();
    auto pipeline = demand.get();
    cache.Stop();
    REQUIRE(immediate);
    REQUIRE(pipeline);
    REQUIRE(*pipeline == 2);
    REQUIRE(demanded_builds == 1);
}

TEST_CASE("Oversized cached snapshots remain indexed for demand without background allocation",
          "[video_core][pipeline_prebuild]") {
    Vulkan::PipelinePrebuild<int, int> cache{1, 100, false};
    std::atomic<int> oversized_builds{};
    REQUIRE(cache.Add(1, 101, [&] {
        ++oversized_builds;
        return std::make_unique<int>(1);
    }));
    std::promise<void> small_entered;
    REQUIRE(cache.Add(2, 1, [&] {
        small_entered.set_value();
        return std::make_unique<int>(2);
    }));
    cache.Start(1);
    small_entered.get_future().wait();
    const bool skipped_background = oversized_builds == 0;
    auto pipeline = cache.Take(1);
    cache.Stop();
    REQUIRE(skipped_background);
    REQUIRE(pipeline);
    REQUIRE(*pipeline == 1);
    REQUIRE(oversized_builds == 1);
}

TEST_CASE("Cached failures release captures and refill without retrying the failed record",
          "[video_core][pipeline_prebuild]") {
    for (int failure = 0; failure < 3; ++failure) {
        Vulkan::PipelinePrebuild<int, int> cache{1, 100, false};
        auto input = std::make_shared<int>(42);
        std::weak_ptr<int> weak = input;
        std::atomic<int> attempts{};
        REQUIRE(cache.Add(1, 100, [&, input = std::move(input)]() -> std::unique_ptr<int> {
            ++attempts;
            if (failure == 0) {
                throw std::runtime_error("cached translation failed");
            }
            if (failure == 1) {
                throw 42;
            }
            return nullptr;
        }));
        std::promise<void> entered;
        REQUIRE(cache.Add(2, 100, [&] {
            entered.set_value();
            return std::make_unique<int>(7);
        }));
        cache.Start(1);
        entered.get_future().wait();
        const bool released = weak.expired();
        auto pipeline = cache.Take(2);
        const bool fallback = !cache.Take(1);
        cache.Stop();
        REQUIRE(released);
        REQUIRE(fallback);
        REQUIRE(pipeline);
        REQUIRE(attempts == 1);
    }
}

TEST_CASE("Prebuild cancellation joins translation and releases queued metadata without refill",
          "[video_core][pipeline_prebuild]") {
    auto resource = std::make_unique<int>(0);
    auto cache = std::make_unique<Vulkan::PipelinePrebuild<int, int>>(1, 100, false);
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    REQUIRE(cache->Add(1, 1, [&, output = resource.get()] {
        entered.set_value();
        gate.wait();
        *output = 42;
        return std::unique_ptr<int>{};
    }));
    auto queued_input = std::make_shared<int>(42);
    std::weak_ptr<int> weak = queued_input;
    std::atomic<int> queued_runs{};
    REQUIRE(cache->Add(2, 1, [&, input = std::move(queued_input)] {
        ++queued_runs;
        return std::make_unique<int>(*input);
    }));
    cache->Start(1);
    entered.get_future().wait();
    auto stop = std::async(std::launch::async, [&] { cache->Stop(); });
    bool admission_closed = false;
    for (int key = 100; key < 2100; ++key) {
        if (!cache->Add(key, 1, [] { return std::make_unique<int>(0); })) {
            admission_closed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    const bool joined = Pending(stop);
    release.set_value();
    stop.get();
    cache.reset();
    REQUIRE(admission_closed);
    REQUIRE(joined);
    REQUIRE(*resource == 42);
    REQUIRE(weak.expired());
    REQUIRE(queued_runs == 0);
}

TEST_CASE("Broken parallel compilation prevents all speculative prebuilds",
          "[video_core][pipeline_prebuild]") {
    Vulkan::PipelinePrebuild<int, int> cache{64, 16 * 1024 * 1024, true};
    bool built = false;
    REQUIRE_FALSE(cache.Add(1, 1, [&] {
        built = true;
        return std::make_unique<int>(42);
    }));
    cache.Start(2);
    cache.Stop();
    REQUIRE_FALSE(built);
}

TEST_CASE("Concurrent prebuild completion and adoption never return a pipeline twice",
          "[video_core][pipeline_prebuild]") {
    for (int iteration = 0; iteration < 32; ++iteration) {
        Vulkan::PipelinePrebuild<int, int> cache{2, 100, false};
        REQUIRE(cache.Add(1, 1, [] { return std::make_unique<int>(42); }));
        cache.Start(2);
        auto first = std::async(std::launch::async, [&] { return cache.Take(1); });
        auto second = std::async(std::launch::async, [&] { return cache.Take(1); });
        const auto a = first.get();
        const auto b = second.get();
        cache.Stop();
        REQUIRE(static_cast<bool>(a) != static_cast<bool>(b));
        REQUIRE((a ? *a : *b) == 42);
    }
}

TEST_CASE("Prebuild accepts new lightweight work after starting and frees adopted slots",
          "[video_core][pipeline_prebuild]") {
    Vulkan::PipelinePrebuild<int, int> cache{1, 100, false};
    cache.Start(1);
    REQUIRE(cache.Add(1, 100, [] { return std::make_unique<int>(1); }));
    auto first = cache.Take(1);
    std::promise<void> entered;
    REQUIRE(cache.Add(2, 100, [&] {
        entered.set_value();
        return std::make_unique<int>(2);
    }));
    entered.get_future().wait();
    auto second = cache.Take(2);
    cache.Stop();
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(*first == 1);
    REQUIRE(*second == 2);
}

TEST_CASE("Stopping prebuild joins a demanded translation on success and failure",
          "[video_core][pipeline_prebuild]") {
    for (const bool fail : {false, true}) {
        Vulkan::PipelinePrebuild<int, int> cache{1, 100, false};
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        auto input = std::make_shared<int>(42);
        std::weak_ptr<int> weak = input;
        // Oversized records can only start when demand claims them.
        REQUIRE(cache.Add(1, 101, [&, input = std::move(input)] {
            entered.set_value();
            gate.wait();
            if (fail) {
                throw std::runtime_error("demanded translation failed");
            }
            return std::make_unique<int>(*input);
        }));
        cache.Start(1);
        auto demand = std::async(std::launch::async, [&] { return cache.Take(1); });
        entered.get_future().wait();
        auto stop = std::async(std::launch::async, [&] { cache.Stop(); });
        const bool shutdown_held = Pending(stop);
        release.set_value();
        auto pipeline = demand.get();
        stop.get();
        REQUIRE(shutdown_held);
        REQUIRE(weak.expired());
        REQUIRE_FALSE(cache.Add(2, 0, [] { return std::make_unique<int>(2); }));
        if (fail) {
            REQUIRE_FALSE(pipeline);
        }
    }
}
