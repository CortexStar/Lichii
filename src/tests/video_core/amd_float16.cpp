// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/vulkan_device.h"

TEST_CASE("AMD float16 stays off only before driver 2.0.416", "[video_core][vulkan]") {
    using Vulkan::IsAmdFloat16Broken;
    // 2.0.416 is 26.10.2 (8389024 in vulkaninfo); 2.0.353 an earlier RDNA2 driver.
    REQUIRE(VK_MAKE_API_VERSION(0, 2, 0, 416) == 8389024U);
    REQUIRE(IsAmdFloat16Broken(VK_MAKE_API_VERSION(0, 2, 0, 353)));
    REQUIRE(IsAmdFloat16Broken(VK_MAKE_API_VERSION(0, 2, 0, 415)));
    REQUIRE_FALSE(IsAmdFloat16Broken(VK_MAKE_API_VERSION(0, 2, 0, 416)));
    REQUIRE_FALSE(IsAmdFloat16Broken(VK_MAKE_API_VERSION(0, 2, 0, 430)));
    REQUIRE_FALSE(IsAmdFloat16Broken(VK_MAKE_API_VERSION(0, 2, 1, 0)));
}
