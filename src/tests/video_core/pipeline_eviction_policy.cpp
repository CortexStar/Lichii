// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_eviction_policy.h"

TEST_CASE("Switch 2 pipeline-count target uses one sixteenth of host RAM",
          "[video_core][pipeline_eviction]") {
    constexpr u64 GiB = u64{1} << 30;
    STATIC_REQUIRE(Vulkan::Switch2PipelineCountTarget(0) == 0);
    STATIC_REQUIRE(
        Vulkan::Switch2PipelineCountTarget(16 * Vulkan::GraphicsPipelineMemoryEstimate - 1) == 0);
    STATIC_REQUIRE(
        Vulkan::Switch2PipelineCountTarget(16 * Vulkan::GraphicsPipelineMemoryEstimate) == 1);
    CHECK(Vulkan::Switch2PipelineCountTarget(16 * GiB) == 2386);
    CHECK(Vulkan::Switch2PipelineCountTarget(32 * GiB) == 4772);
}

TEST_CASE("Pipeline eviction approaches the target only with eligible stale pipelines",
          "[video_core][pipeline_eviction]") {
    CHECK(Vulkan::GraphicsPipelinesToEvict(100, 100, 100) == 0);
    CHECK(Vulkan::GraphicsPipelinesToEvict(150, 50, 100) == 50);
    CHECK(Vulkan::GraphicsPipelinesToEvict(150, 20, 100) == 20);
    CHECK(Vulkan::GraphicsPipelinesToEvict(150, 0, 100) == 0);
}

TEST_CASE("Pipeline eviction selects the oldest count and shares cutoff ties",
          "[video_core][pipeline_eviction]") {
    std::array<u64, 4> last_uses{100, 200, 200, 400};
    const std::vector<u64> expected_ages{100, 200};
    do {
        std::array<Vulkan::EvictionCandidate, 4> candidates{};
        for (size_t i = 0; i < candidates.size(); ++i) {
            candidates[i] = {last_uses[i], true};
        }
        const std::vector<size_t> selected = Vulkan::SelectPipelinesToEvict(candidates, 0, 2);
        REQUIRE(selected.size() == 2);
        std::vector<u64> selected_ages;
        for (const size_t index : selected) {
            selected_ages.push_back(candidates[index].last_use);
        }
        std::ranges::sort(selected_ages);
        CHECK(selected_ages == expected_ages);
    } while (std::next_permutation(last_uses.begin(), last_uses.end()));

    const std::array<Vulkan::EvictionCandidate, 4> all_candidates{{
        {100, true},
        {200, true},
        {200, true},
        {400, true},
    }};
    CHECK(Vulkan::SelectPipelinesToEvict(all_candidates, 0, 0).empty());
    CHECK(Vulkan::SelectPipelinesToEvict(all_candidates, 0, all_candidates.size()).size() ==
          all_candidates.size());
}

TEST_CASE("Pipeline eviction protects up to 2,000 unused prebuilt pipelines",
          "[video_core][pipeline_eviction]") {
    std::vector<Vulkan::EvictionCandidate> prebuilt(2001, {0, false});
    CHECK(Vulkan::SelectPipelinesToEvict(prebuilt, 2000, prebuilt.size()) ==
          std::vector<size_t>{2000});

    const std::array<Vulkan::EvictionCandidate, 2> frame_zero{{{0, true}, {0, false}}};
    CHECK(Vulkan::SelectPipelinesToEvict(frame_zero, 1, 1) == std::vector<size_t>{0});
}
