// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"
#include "video_core/vulkan_common/vulkan_device.h"

TEST_CASE("Graphics library links are optimized on AMD's driver and without fast linking",
          "[video_core][vulkan][gpl]") {
    using Vulkan::GraphicsLibraryLinksOptimized;
    // Fast links draw fringes on AMD's Windows driver even where it reports fast linking.
    REQUIRE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_AMD_PROPRIETARY, true));
    REQUIRE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_AMD_PROPRIETARY, false));
    REQUIRE_FALSE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true));
    REQUIRE_FALSE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_MESA_RADV, true));
    REQUIRE(GraphicsLibraryLinksOptimized(VK_DRIVER_ID_NVIDIA_PROPRIETARY, false));
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
