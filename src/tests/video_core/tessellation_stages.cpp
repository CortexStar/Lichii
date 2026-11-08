// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/tessellation_stages.h"

TEST_CASE("Tessellation stages come from the tessellation slots of non-mesh pipelines",
          "[video_core][vulkan][tessellation]") {
    using Vulkan::HasTessellationStages;
    CHECK_FALSE(HasTessellationStages(false, false, false));
    CHECK(HasTessellationStages(false, true, false));
    CHECK(HasTessellationStages(false, false, true));
    CHECK(HasTessellationStages(false, true, true));
    // Task + mesh: the mesh program sits in a tessellation slot.
    CHECK_FALSE(HasTessellationStages(true, true, false));
    CHECK_FALSE(HasTessellationStages(true, false, true));
    CHECK_FALSE(HasTessellationStages(true, false, false));
}
