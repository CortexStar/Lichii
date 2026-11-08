// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/oot_dlss.h"

TEST_CASE("OoT AMD upscaler: FSR 4 first, FSR 3.1 on the tonemapped image only",
          "[video_core][oot]") {
    using VideoCommon::OotDlss::AmdUpscaler;
    using VideoCommon::OotDlss::PickAmdUpscaler;
    CHECK(PickAmdUpscaler(true, true, true) == AmdUpscaler::Fsr4);
    CHECK(PickAmdUpscaler(true, false, false) == AmdUpscaler::Fsr4);
    CHECK(PickAmdUpscaler(false, false, true) == AmdUpscaler::Fsr31);
    CHECK(PickAmdUpscaler(false, true, true) == AmdUpscaler::None);
    CHECK(PickAmdUpscaler(false, false, false) == AmdUpscaler::None);
}
