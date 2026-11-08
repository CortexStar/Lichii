// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/cached_pipeline_policy.h"

using Vulkan::CachedPipelineCounts;
using Vulkan::CachedPipelinePlacement;
using Vulkan::MakeCachedPipelinePolicy;
using Vulkan::PlaceCachedPipeline;

TEST_CASE("Recently used cached pipelines are built at boot", "[video_core][pipeline_cache]") {
    const auto policy = MakeCachedPipelinePolicy(false, true, 3000, true, 9000);
    CHECK(PlaceCachedPipeline(policy, true, {0, 0}) == CachedPipelinePlacement::Boot);
    // The budget orders the rest; the used ones are built whatever the count.
    CHECK(PlaceCachedPipeline(policy, true, {9000, 0}) == CachedPipelinePlacement::Boot);
}

TEST_CASE("A Switch 2 title builds every other cached pipeline in the background up to its budget",
          "[video_core][pipeline_cache]") {
    const auto policy = MakeCachedPipelinePolicy(false, true, 3000, true, 2300);
    // Ocarina of Time on the bench PC: 1,587 at boot, 644 more cached.
    CHECK(PlaceCachedPipeline(policy, false, {1587, 0}) == CachedPipelinePlacement::Background);
    CHECK(PlaceCachedPipeline(policy, false, {1587, 64}) == CachedPipelinePlacement::Background);
    CHECK(PlaceCachedPipeline(policy, false, {1587, 643}) == CachedPipelinePlacement::Background);
    // Past the budget, boot and background together, the rest wait for a draw.
    CHECK(PlaceCachedPipeline(policy, false, {1587, 713}) == CachedPipelinePlacement::Skip);
    CHECK(PlaceCachedPipeline(policy, false, {2300, 0}) == CachedPipelinePlacement::Skip);
}

TEST_CASE("Titles without a pipeline budget keep the short background prebuild",
          "[video_core][pipeline_cache]") {
    const auto policy = MakeCachedPipelinePolicy(false, true, 3000, false, 9000);
    CHECK(PlaceCachedPipeline(policy, false, {100, 0}) == CachedPipelinePlacement::Background);
    CHECK(PlaceCachedPipeline(policy, false, {100, Vulkan::LegacyBackgroundPrebuildLimit - 1}) ==
          CachedPipelinePlacement::Background);
    CHECK(PlaceCachedPipeline(policy, false, {100, Vulkan::LegacyBackgroundPrebuildLimit}) ==
          CachedPipelinePlacement::Skip);
}

TEST_CASE("Without a used list the first pipelines are built at boot, the rest after",
          "[video_core][pipeline_cache]") {
    const auto policy = MakeCachedPipelinePolicy(false, false, 3000, true, 4600);
    CHECK(PlaceCachedPipeline(policy, false, {2999, 0}) == CachedPipelinePlacement::Boot);
    CHECK(PlaceCachedPipeline(policy, false, {3000, 0}) == CachedPipelinePlacement::Background);
    CHECK(PlaceCachedPipeline(policy, false, {3000, 1599}) == CachedPipelinePlacement::Background);
    CHECK(PlaceCachedPipeline(policy, false, {3000, 1600}) == CachedPipelinePlacement::Skip);
}

TEST_CASE("Preloading builds every cached pipeline at boot", "[video_core][pipeline_cache]") {
    const auto policy = MakeCachedPipelinePolicy(true, true, 3000, true, 10);
    CHECK(PlaceCachedPipeline(policy, false, {0, 0}) == CachedPipelinePlacement::Boot);
    CHECK(PlaceCachedPipeline(policy, false, {std::numeric_limits<size_t>::max() - 1, 0}) ==
          CachedPipelinePlacement::Boot);
}
