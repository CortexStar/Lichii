// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "video_core/vulkan_common/driver_crash.h"

namespace {
using namespace Vulkan;

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream file{path};
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

template <typename T>
T FakeHandle(uintptr_t value) {
    return reinterpret_cast<T>(value);
}
} // Anonymous namespace

TEST_CASE("Driver crash: a link dumps the SPIR-V of the libraries it links",
          "[video_core][diagnostics]") {
    const std::array<u32, 5> vertex{0x07230203, 0x00010600, 0, 1, 0};
    const std::array<u32, 5> fragment{0x07230203, 0x00010600, 0, 2, 0};
    const auto vertex_module = FakeHandle<VkShaderModule>(0x1001);
    const auto fragment_module = FakeHandle<VkShaderModule>(0x1002);
    DriverCrash::RegisterModule(vertex_module, vertex);
    DriverCrash::RegisterModule(fragment_module, fragment);

    // Two libraries, one stage each, made while their modules lived.
    const VkGraphicsPipelineLibraryCreateInfoEXT library_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    };
    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex_module,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment_module,
         .pName = "main"},
    }};
    const auto vertex_library = FakeHandle<VkPipeline>(0x2001);
    const auto fragment_library = FakeHandle<VkPipeline>(0x2002);
    for (size_t i = 0; i < stages.size(); ++i) {
        const VkGraphicsPipelineCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &library_ci,
            .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
            .stageCount = 1,
            .pStages = &stages[i],
        };
        DriverCrash::RegisterPipeline(i == 0 ? vertex_library : fragment_library, ci);
    }
    // Lichii frees the modules once the libraries exist.
    DriverCrash::ForgetModule(vertex_module);
    DriverCrash::ForgetModule(fragment_module);

    const std::array<VkPipeline, 2> libraries{vertex_library, fragment_library};
    const VkPipelineLibraryCreateInfoKHR link_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
        .libraryCount = static_cast<u32>(libraries.size()),
        .pLibraries = libraries.data(),
    };
    const VkGraphicsPipelineCreateInfo link{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &link_ci,
        .flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT,
    };
    const DriverCall::Record record{"vkCreateGraphicsPipelines", DriverCall::Kind::GraphicsPipeline,
                                    1, &link, nullptr};
    const std::array<u64, 6> hashes{0xaaaa, 0, 0, 0, 0, 0xbbbb};
    const DriverCall::Context context{"optimized link of graphics pipeline", 0x1234, hashes,
                                      nullptr};

    const auto directory = std::filesystem::temp_directory_path() / "lichii-driver-crash-test";
    std::filesystem::remove_all(directory);
    const std::string folder =
        DriverCrash::WriteArtifact(directory.string(), record, &context, "test fault");
    REQUIRE(!folder.empty());

    const std::string summary = ReadText(std::filesystem::path{folder} / "summary.txt");
    CHECK(summary.starts_with("test fault\n"));
    CHECK(summary.find("in vkCreateGraphicsPipelines(") != std::string::npos);
    CHECK(summary.find("links=2") != std::string::npos);
    CHECK(summary.find("for optimized link of graphics pipeline 0000000000001234 "
                       "s0:000000000000aaaa s5:000000000000bbbb") != std::string::npos);
    const auto vertex_name = fmt::format("0-VERTEX-{:016x}.spv", DriverCall::SpirvHash(vertex));
    const auto fragment_name =
        fmt::format("1-FRAGMENT-{:016x}.spv", DriverCall::SpirvHash(fragment));
    CHECK(summary.find("spirv " + vertex_name) != std::string::npos);
    CHECK(summary.find("spirv " + fragment_name) != std::string::npos);
    CHECK(std::filesystem::file_size(std::filesystem::path{folder} / vertex_name) ==
          vertex.size() * sizeof(u32));
    const std::string state = ReadText(std::filesystem::path{folder} / "state.txt");
    CHECK(state.find("linked library 0x2001") != std::string::npos);
    CHECK(state.find("pNext VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR") !=
          std::string::npos);

    DriverCrash::ForgetPipeline(vertex_library);
    DriverCrash::ForgetPipeline(fragment_library);
    std::filesystem::remove_all(directory);
}

TEST_CASE("Driver crash: pipeline state names stages and specialization constants",
          "[video_core][diagnostics]") {
    const u32 value = 7;
    const VkSpecializationMapEntry entry{.constantID = 3, .offset = 0, .size = sizeof(u32)};
    const VkSpecializationInfo spec{
        .mapEntryCount = 1, .pMapEntries = &entry, .dataSize = sizeof(value), .pData = &value};
    const VkComputePipelineCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = FakeHandle<VkShaderModule>(0x3001),
                .pName = "main",
                .pSpecializationInfo = &spec,
            },
    };
    const DriverCall::Record record{"vkCreateComputePipelines", DriverCall::Kind::ComputePipeline,
                                    1, &ci, nullptr};
    std::FILE* file = std::tmpfile();
    REQUIRE(file != nullptr);
    DriverCrash::WriteState(file, record);
    std::rewind(file);
    std::string text(4096, '\0');
    text.resize(std::fread(text.data(), 1, text.size(), file));
    std::fclose(file);
    CHECK(text.find("stage COMPUTE flags=0x0 module=0x3001 entry=main") != std::string::npos);
    CHECK(text.find("spec constant 3 = 0x7 (4 bytes)") != std::string::npos);
}
