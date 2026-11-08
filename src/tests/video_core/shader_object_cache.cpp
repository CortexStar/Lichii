// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_shader_object.h"

namespace {
using namespace Vulkan::ShaderObject;

Key MakeKey(u64 vertex_hash, u64 fragment_hash) {
    Key key{};
    key.spirv_hashes[0] = {vertex_hash, 1};
    key.stage_flags[0] = VK_SHADER_STAGE_VERTEX_BIT;
    key.spirv_hashes[4] = {fragment_hash, 2};
    key.stage_flags[4] = VK_SHADER_STAGE_FRAGMENT_BIT;
    return key;
}
} // Anonymous namespace

TEST_CASE("Linked stages name the stage after them, the last none", "[shader_object]") {
    const StageChain chain =
        ChainStages({VK_SHADER_STAGE_VERTEX_BIT, VkShaderStageFlagBits{}, VkShaderStageFlagBits{},
                     VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT});
    REQUIRE(chain.count == 3);
    REQUIRE(chain.stages[0] == VK_SHADER_STAGE_VERTEX_BIT);
    REQUIRE(chain.next[0] == VK_SHADER_STAGE_GEOMETRY_BIT);
    REQUIRE(chain.stages[1] == VK_SHADER_STAGE_GEOMETRY_BIT);
    REQUIRE(chain.next[1] == VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE(chain.stages[2] == VK_SHADER_STAGE_FRAGMENT_BIT);
    REQUIRE(chain.next[2] == 0);

    const StageChain mesh =
        ChainStages({VK_SHADER_STAGE_MESH_BIT_EXT, VkShaderStageFlagBits{}, VkShaderStageFlagBits{},
                     VkShaderStageFlagBits{}, VK_SHADER_STAGE_FRAGMENT_BIT});
    REQUIRE(mesh.count == 2);
    REQUIRE(mesh.next[0] == VK_SHADER_STAGE_FRAGMENT_BIT);
}

TEST_CASE("Every graphics stage has a bind slot", "[shader_object]") {
    REQUIRE(BindIndex(VK_SHADER_STAGE_VERTEX_BIT) == 0);
    REQUIRE(BindIndex(VK_SHADER_STAGE_FRAGMENT_BIT) == 4);
    REQUIRE(BindIndex(VK_SHADER_STAGE_MESH_BIT_EXT) == 6);
    REQUIRE(BindIndex(VK_SHADER_STAGE_COMPUTE_BIT) == BIND_STAGES.size());
    REQUIRE(NumBindStages(false) == 5);
    REQUIRE(NumBindStages(true) == 7);
}

TEST_CASE("Pipelines with the same shaders share one set", "[shader_object]") {
    Cache cache;
    int builds = 0;
    const auto build = [&builds] {
        ++builds;
        return std::make_shared<const Set>();
    };
    const auto first = cache.Get(MakeKey(1, 2), build);
    const auto second = cache.Get(MakeKey(1, 2), build);
    REQUIRE(builds == 1);
    REQUIRE(first == second);
    const auto other = cache.Get(MakeKey(1, 3), build);
    REQUIRE(builds == 2);
    REQUIRE(other != first);
    REQUIRE(cache.Size() == 2);
}

TEST_CASE("A set no pipeline holds any more is built again", "[shader_object]") {
    Cache cache;
    int builds = 0;
    const auto build = [&builds] {
        ++builds;
        return std::make_shared<const Set>();
    };
    {
        const auto set = cache.Get(MakeKey(7, 8), build);
        REQUIRE(cache.Size() == 1);
    }
    REQUIRE(cache.Size() == 0);
    (void)cache.Get(MakeKey(7, 8), build);
    REQUIRE(builds == 2);
}

TEST_CASE("A failed build is not cached", "[shader_object]") {
    Cache cache;
    REQUIRE(cache.Get(MakeKey(1, 1), [] { return std::shared_ptr<const Set>{}; }) == nullptr);
    int builds = 0;
    (void)cache.Get(MakeKey(1, 1), [&builds] {
        ++builds;
        return std::make_shared<const Set>();
    });
    REQUIRE(builds == 1);
}
