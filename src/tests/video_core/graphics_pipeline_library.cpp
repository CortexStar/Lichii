// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/object_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"
#include "video_core/vulkan_common/vulkan_device.h"

TEST_CASE("Graphics library links are optimized only where AMD's fast links leak demoted pixels",
          "[video_core][vulkan][gpl]") {
    using Vulkan::GraphicsLibrariesKeepLinkInfo;
    using Vulkan::GraphicsLibraryLinksOptimized;
    // GPUOpen-Drivers/AMD-Gfx-Drivers#126: quad reads after a demote need an optimized link there.
    REQUIRE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_AMD_PROPRIETARY, true, true));
    REQUIRE_FALSE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_AMD_PROPRIETARY, true, false));
    REQUIRE_FALSE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true, true));
    REQUIRE_FALSE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_MESA_RADV, true, true));
    REQUIRE_FALSE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_AMD_OPEN_SOURCE, true, true));
    // Without fast linking every link is optimized.
    for (const bool demote : {false, true}) {
        REQUIRE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_AMD_PROPRIETARY, false, demote));
        REQUIRE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_NVIDIA_PROPRIETARY, false, demote));
    }
    // Libraries keep link time optimization info wherever any link may be optimized.
    for (const VkDriverId driver :
         {VK_DRIVER_ID_AMD_PROPRIETARY, VK_DRIVER_ID_NVIDIA_PROPRIETARY, VK_DRIVER_ID_MESA_RADV}) {
        for (const bool fast_linking : {false, true}) {
            for (const bool demote : {false, true}) {
                if (GraphicsLibraryLinksOptimized(driver, fast_linking, demote)) {
                    REQUIRE(GraphicsLibrariesKeepLinkInfo(driver, fast_linking));
                }
            }
        }
    }
    REQUIRE_FALSE(GraphicsLibrariesKeepLinkInfo(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true));
}

namespace {
namespace IR = Shader::IR;

struct DemotePrograms {
    Shader::ObjectPool<IR::Inst> instructions;
    Shader::ObjectPool<IR::Block> blocks;
    IR::Program program;

    IR::Block* Add() {
        IR::Block* const block = blocks.Create(instructions);
        program.blocks.push_back(block);
        return block;
    }

    static void Demote(IR::Block* block) {
        block->AppendNewInst(IR::Opcode::DemoteToHelperInvocation, {});
    }

    static void Derivative(IR::Block* block) {
        block->AppendNewInst(IR::Opcode::DPdxFine, {IR::Value{1.0f}});
    }
};
} // namespace

TEST_CASE("Quad reads count only when they can run after a demote", "[shader][gpl]") {
    using Shader::Optimization::UsesQuadAfterDemote;
    SECTION("same block, derivative after the demote") {
        DemotePrograms p;
        IR::Block* const block = p.Add();
        DemotePrograms::Demote(block);
        DemotePrograms::Derivative(block);
        REQUIRE(UsesQuadAfterDemote(p.program));
    }
    SECTION("same block, derivative before the demote") {
        DemotePrograms p;
        IR::Block* const block = p.Add();
        DemotePrograms::Derivative(block);
        DemotePrograms::Demote(block);
        REQUIRE_FALSE(UsesQuadAfterDemote(p.program));
    }
    SECTION("derivative in a block the demote branches to") {
        DemotePrograms p;
        IR::Block* const entry = p.Add();
        IR::Block* const discard = p.Add();
        IR::Block* const merge = p.Add();
        entry->AddBranch(discard);
        entry->AddBranch(merge);
        discard->AddBranch(merge);
        DemotePrograms::Demote(discard);
        DemotePrograms::Derivative(merge);
        REQUIRE(UsesQuadAfterDemote(p.program));
    }
    SECTION("derivative only on a path the demote can't reach") {
        DemotePrograms p;
        IR::Block* const entry = p.Add();
        IR::Block* const discard = p.Add();
        IR::Block* const shade = p.Add();
        entry->AddBranch(discard);
        entry->AddBranch(shade);
        DemotePrograms::Demote(discard);
        DemotePrograms::Derivative(shade);
        DemotePrograms::Derivative(entry);
        REQUIRE_FALSE(UsesQuadAfterDemote(p.program));
    }
    SECTION("derivative earlier in a loop body that demotes") {
        DemotePrograms p;
        IR::Block* const entry = p.Add();
        IR::Block* const body = p.Add();
        IR::Block* const exit = p.Add();
        entry->AddBranch(body);
        body->AddBranch(body);
        body->AddBranch(exit);
        DemotePrograms::Derivative(body);
        DemotePrograms::Demote(body);
        REQUIRE(UsesQuadAfterDemote(p.program));
    }
    SECTION("an implicit-LOD sample after the demote") {
        DemotePrograms p;
        IR::Block* const block = p.Add();
        DemotePrograms::Demote(block);
        block->AppendNewInst(IR::Opcode::ImageSampleImplicitLod,
                             {IR::Value{0u}, IR::Value{0.0f}, IR::Value{0u}, IR::Value{0u}});
        REQUIRE(UsesQuadAfterDemote(p.program));
    }
    SECTION("no demote") {
        DemotePrograms p;
        DemotePrograms::Derivative(p.Add());
        REQUIRE_FALSE(UsesQuadAfterDemote(p.program));
    }
}

TEST_CASE("Library pipelines keep each side within its rescaling bits",
          "[video_core][vulkan][gpl]") {
    using Vulkan::FitsLibraryRescalingBits;
    using Vulkan::GPL_FRAGMENT_IMAGE_SCALING_BASE;
    using Vulkan::GPL_FRAGMENT_TEXTURE_SCALING_BASE;
    // The fragment side starts at the base and must end within the push constant's bits.
    static_assert(GPL_FRAGMENT_TEXTURE_SCALING_BASE * 2 ==
                  Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS * 32);
    static_assert(GPL_FRAGMENT_IMAGE_SCALING_BASE * 2 ==
                  Shader::Backend::SPIRV::NUM_IMAGE_SCALING_WORDS * 32);
    REQUIRE(FitsLibraryRescalingBits(0, 0, 0, 0));
    REQUIRE(FitsLibraryRescalingBits(64, 32, 64, 32));
    REQUIRE_FALSE(FitsLibraryRescalingBits(65, 0, 0, 0));
    REQUIRE_FALSE(FitsLibraryRescalingBits(0, 33, 0, 0));
    REQUIRE_FALSE(FitsLibraryRescalingBits(0, 0, 65, 0));
    REQUIRE_FALSE(FitsLibraryRescalingBits(0, 0, 0, 33));
}
