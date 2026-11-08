// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/heap_mappings.h"

namespace {
using namespace Vulkan::HeapMappings;

Shader::TextureDescriptor Texture(u32 count) {
    Shader::TextureDescriptor desc{};
    desc.count = count;
    return desc;
}

std::array<Shader::Info, 5> TwoStages() {
    std::array<Shader::Info, 5> infos{};
    infos[1].texture_descriptors.push_back(Texture(1));
    infos[4].texture_descriptors.push_back(Texture(1));
    infos[4].texture_descriptors.push_back(Texture(4));
    return infos;
}
} // Anonymous namespace

TEST_CASE("Heap mappings number monolithic bindings over the stages", "[heap_mappings]") {
    const auto infos = TwoStages();
    const TableLayout layout = MakeTableLayout(infos, false);
    REQUIRE(layout.stage_elements[1] == 1);
    REQUIRE(layout.stage_elements[4] == 5);
    REQUIRE(layout.bindings.size() == 3);
    REQUIRE(layout.bindings[0].set == 0);
    REQUIRE(layout.bindings[0].binding == 0);
    REQUIRE(layout.bindings[1].stage == 4);
    REQUIRE(layout.bindings[1].binding == 1);
    REQUIRE(layout.bindings[2].binding == 2);
    // Elements count per stage: a stage's table doesn't depend on the others.
    REQUIRE(layout.bindings[2].first_element == 1);
    REQUIRE(layout.TableBytes(4) == 40);
}

TEST_CASE("Heap mappings of a stage don't depend on the other stages", "[heap_mappings]") {
    auto infos = TwoStages();
    const auto before = MakeStageMappings(MakeTableLayout(infos, true));
    infos[1].texture_descriptors.push_back(Texture(3)); // the vertex stage changes
    const auto after = MakeStageMappings(MakeTableLayout(infos, true));
    REQUIRE(before[4].size() == after[4].size());
    for (size_t i = 0; i < before[4].size(); ++i) {
        const auto& a = before[4][i].sourceData.indirectIndexArray;
        const auto& b = after[4][i].sourceData.indirectIndexArray;
        REQUIRE(a.addressOffset == b.addressOffset);
        REQUIRE(a.samplerAddressOffset == b.samplerAddressOffset);
        REQUIRE(before[4][i].firstBinding == after[4][i].firstBinding);
    }
}

TEST_CASE("Heap mappings give libraries one set per stage", "[heap_mappings]") {
    const auto infos = TwoStages();
    const TableLayout layout = MakeTableLayout(infos, true);
    REQUIRE(layout.bindings[0].set == 1);
    REQUIRE(layout.bindings[0].binding == 0);
    REQUIRE(layout.bindings[1].set == 4);
    REQUIRE(layout.bindings[1].binding == 0);
    REQUIRE(layout.bindings[2].binding == 1);
}

TEST_CASE("Heap texture mappings read images then samplers from the table", "[heap_mappings]") {
    const auto infos = TwoStages();
    const TableLayout layout = MakeTableLayout(infos, false);
    const auto stages = MakeStageMappings(layout);
    REQUIRE(stages[1].size() == 1);
    REQUIRE(stages[4].size() == 2);
    const auto& array = stages[4][1].sourceData.indirectIndexArray;
    REQUIRE(stages[4][1].source == VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT);
    REQUIRE(stages[4][1].firstBinding == 2);
    REQUIRE(array.addressOffset == 1 * 4);
    REQUIRE(array.samplerAddressOffset == (5 + 1) * 4);
    REQUIRE(array.heapIndexStride == 32);
    REQUIRE(array.samplerHeapIndexStride == 16);
    REQUIRE(array.pushOffset == TablePushOffset(4));
    REQUIRE(array.samplerPushOffset == array.pushOffset);
    REQUIRE(array.samplerPushOffset % 8 == 0);
    REQUIRE(stages[1][0].sourceData.indirectIndexArray.pushOffset == TablePushOffset(1));
    REQUIRE(PUSH_DATA_SIZE <= 512);
}

TEST_CASE("Heap mode needs every other resource by device address", "[heap_mappings]") {
    auto infos = TwoStages();
    REQUIRE(Eligible(infos, false));
    REQUIRE_FALSE(Eligible(infos, true));
    infos[4].constant_buffer_descriptors.push_back({});
    REQUIRE_FALSE(Eligible(infos, false));
    infos[4].uniforms_by_address = true;
    REQUIRE(Eligible(infos, false));
    infos[1].image_descriptors.push_back({});
    REQUIRE_FALSE(Eligible(infos, false));
}
