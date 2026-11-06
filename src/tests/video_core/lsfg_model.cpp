// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <set>
#include <catch2/catch_test_macros.hpp>
#include "common/settings.h"
#include "video_core/frame_gen/lsfg_model.h"

TEST_CASE("LSFG 3.1 standard quality selects its own DLL network", "[lsfg]") {
    using namespace VideoCore::FrameGen;
    std::set<u32> standard, performance;
    for (u32 id = 280; id <= 302; ++id) {
        const auto quality_id = LsfgShaderId(id, false);
        REQUIRE(quality_id >= 257);
        REQUIRE(quality_id <= 279);
        REQUIRE(LsfgShaderId(id, true) == id);
        standard.insert(quality_id);
        performance.insert(LsfgShaderId(id, true));
    }
    REQUIRE(standard.size() == 23);
    REQUIRE(performance.size() == 23);
    for (u32 shared : {255U, 256U}) {
        REQUIRE(LsfgShaderId(shared, false) == shared);
        REQUIRE(LsfgShaderId(shared, true) == shared);
    }
    REQUIRE(LsfgFeaturePlanes(false) == 4);
    REQUIRE(LsfgFeaturePlanes(true) == 2);
    REQUIRE_FALSE(Settings::values.frame_gen_performance_mode.GetDefault());
}
