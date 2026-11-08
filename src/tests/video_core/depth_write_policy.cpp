// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/depth_write_policy.h"

TEST_CASE("Only AMD single-sample depth attachments are written by shaders",
          "[video_core][vulkan][depth_write_policy]") {
    using Vulkan::DepthWrittenByShader;
    constexpr auto Amd = VK_DRIVER_ID_AMD_PROPRIETARY;
    CHECK(DepthWrittenByShader(Amd, true, true, 1, true));
    // Colour and stencil-only formats, MSAA and non-attachable images keep their transfers.
    CHECK_FALSE(DepthWrittenByShader(Amd, true, true, 1, false));
    CHECK_FALSE(DepthWrittenByShader(Amd, true, true, 4, true));
    CHECK_FALSE(DepthWrittenByShader(Amd, true, false, 1, true));
    // Without stencil export a depth-stencil upload could not write stencil.
    CHECK_FALSE(DepthWrittenByShader(Amd, false, true, 1, true));
    // Other drivers keep the transfer paths: the GENERAL layout rule above is AMD's driver's.
    CHECK_FALSE(DepthWrittenByShader(VK_DRIVER_ID_MESA_RADV, true, true, 1, true));
    CHECK_FALSE(DepthWrittenByShader(VK_DRIVER_ID_NVIDIA_PROPRIETARY, true, true, 1, true));
}
