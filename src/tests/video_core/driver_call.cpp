// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "video_core/vulkan_common/driver_call.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {
using namespace Vulkan;

bool Contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}
} // Anonymous namespace

TEST_CASE("Driver call: a buffer create names its size and usage", "[video_core][diagnostics]") {
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 0x10000,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    CHECK(DriverCall::DescribeCurrent().empty());
    {
        const DriverCall::Scope call{"vmaCreateBuffer", DriverCall::Kind::Buffer, &ci};
        const std::string text = DriverCall::DescribeCurrent();
        CHECK(Contains(text, "vmaCreateBuffer("));
        CHECK(Contains(text, "size=0x10000"));
        CHECK(Contains(text, "STORAGE_BUFFER|SHADER_DEVICE_ADDRESS"));
    }
    CHECK(DriverCall::Current() == nullptr);
}

TEST_CASE("Driver call: nested calls and the guest pipeline they build",
          "[video_core][diagnostics]") {
    const std::array<u64, 6> hashes{0x1111, 0, 0, 0, 0, 0x5555};
    const std::array<u32, 5> words{0x07230203, 0x00010600, 0, 1, 0};
    const VkShaderModuleCreateInfo module_ci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = words.size() * sizeof(u32),
        .pCode = words.data(),
    };
    const DriverCall::ContextScope context{"graphics pipeline", 0xabcdef, hashes};
    {
        const DriverCall::Scope outer{"vkCreateGraphicsPipelines", DriverCall::Kind::Other,
                                      nullptr};
        {
            const DriverCall::Scope inner{"vkCreateShaderModule", DriverCall::Kind::ShaderModule,
                                          &module_ci};
            const std::string text = DriverCall::DescribeCurrent();
            CHECK(Contains(text, "vkCreateShaderModule("));
            CHECK(Contains(text, "words=5"));
            CHECK(Contains(text, fmt::format("spirv={:016x}", DriverCall::SpirvHash(words))));
            CHECK(Contains(text, "for graphics pipeline 0000000000abcdef"));
            CHECK(Contains(text, "s0:0000000000001111 s5:0000000000005555"));
        }
        CHECK(Contains(DriverCall::DescribeCurrent(), "vkCreateGraphicsPipelines("));
    }
    CHECK(DriverCall::DescribeCurrent() ==
          "for graphics pipeline 0000000000abcdef s0:0000000000001111 s5:0000000000005555");
}

TEST_CASE("Driver call: descriptions stay inside a short buffer", "[video_core][diagnostics]") {
    const VkImageCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {1280, 720, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    };
    const DriverCall::Record record{"vmaCreateImage", DriverCall::Kind::Image, 1, &ci, nullptr};
    std::array<char, 1024> full{};
    const size_t length = DriverCall::Describe(full.data(), full.size(), &record, nullptr);
    CHECK(std::string(full.data(), length) ==
          "vmaCreateImage( 2D 1280x720x1 levels=1 layers=1 samples=1 R8G8B8A8_UNORM "
          "usage=SAMPLED|COLOR_ATTACHMENT tiling=OPTIMAL )");
    std::array<char, 16> small{};
    small.fill('x');
    CHECK(DriverCall::Describe(small.data(), small.size(), &record, nullptr) == 15);
    CHECK(small.back() == '\0');
}

TEST_CASE("Driver call: function names from std::source_location", "[video_core][diagnostics]") {
    using DriverCall::ShortFunctionName;
    CHECK(ShortFunctionName("Vulkan::vk::Buffer Vulkan::vk::Device::CreateBuffer(const "
                            "VkBufferCreateInfo&) const") == "Device::CreateBuffer");
    CHECK(ShortFunctionName("void Vulkan::Scheduler::SubmitExecution(VkSemaphore)") ==
          "Scheduler::SubmitExecution");
    CHECK(ShortFunctionName("int main()") == "main");
}

TEST_CASE("vk::Exception names the call and the object it was creating",
          "[video_core][diagnostics]") {
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 0x200,
        .usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    };
    const DriverCall::Scope call{"vmaCreateBuffer", DriverCall::Kind::Buffer, &ci};
    const vk::Exception exception{VK_ERROR_INITIALIZATION_FAILED};
    const std::string what = exception.what();
    CHECK(what.starts_with("VK_ERROR_INITIALIZATION_FAILED at "));
    CHECK(Contains(what, "driver_call.cpp:"));
    CHECK(Contains(what, " in vmaCreateBuffer( size=0x200 usage=SHADER_DEVICE_ADDRESS )"));
}
