// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/spirv_check.h"
#include "video_core/vulkan_common/vulkan.h"

TEST_CASE("SPIR-V check: Vulkan versions map to SPIRV-Tools target environments",
          "[video_core][diagnostics]") {
    using Vulkan::SpirvCheck::TargetEnv;
    CHECK(TargetEnv(VK_API_VERSION_1_0) == 1);
    CHECK(TargetEnv(VK_API_VERSION_1_1) == 18);
    CHECK(TargetEnv(VK_API_VERSION_1_2) == 23);
    CHECK(TargetEnv(VK_API_VERSION_1_3) == 25);
    CHECK(TargetEnv(VK_API_VERSION_1_4) == 26);
    CHECK(TargetEnv(VK_MAKE_API_VERSION(0, 1, 5, 0)) == 26);
}

// Runs where SPIRV-Tools' shared library is available (EDEN_NX2_SPIRV_TOOLS=<path>).
TEST_CASE("SPIR-V check: the validator rejects a broken module", "[video_core][diagnostics]") {
    using namespace Vulkan::SpirvCheck;
    if (std::getenv("EDEN_NX2_SPIRV_TOOLS") == nullptr) {
        SKIP("EDEN_NX2_SPIRV_TOOLS not set");
    }
    Configure({.api_version = VK_API_VERSION_1_3,
               .uniform_buffer_standard_layout = true,
               .scalar_block_layout = false,
               .workgroup_scalar_block_layout = false,
               .local_size_id = true});
    REQUIRE(Ready());
    // A valid empty compute shader: OpCapability Shader, OpMemoryModel Logical GLSL450,
    // OpEntryPoint GLCompute %main "main", OpExecutionMode LocalSize 1 1 1, void main() {}.
    const std::array<u32, 35> valid{
        0x07230203, 0x00010600, 0,  6,          0, 0x00020011, 1, // OpCapability Shader
        0x0003000e, 0,          1,                                // OpMemoryModel Logical GLSL450
        0x0005000f, 5,          1,  0x6e69616d, 0,    // OpEntryPoint GLCompute %1 "main"
        0x00060010, 1,          17, 1,          1, 1, // OpExecutionMode %1 LocalSize 1 1 1
        0x00020013, 2,                                // %2 = OpTypeVoid
        0x00030021, 3,          2,                    // %3 = OpTypeFunction %2
        0x00050036, 2,          1,  0,          3,    // %1 = OpFunction %2 None %3
        0x000200f8, 4,                                // %4 = OpLabel
        0x000100fd,                                   // OpReturn
        0x00010038,                                   // OpFunctionEnd
    };
    CHECK(Validate(valid).empty());
    // The same without its OpReturn: the label's block has no terminator.
    std::array<u32, 34> broken{};
    std::copy(valid.begin(), valid.begin() + 33, broken.begin());
    broken[33] = 0x00010038; // OpFunctionEnd
    CHECK(!Validate(broken).empty());
}
