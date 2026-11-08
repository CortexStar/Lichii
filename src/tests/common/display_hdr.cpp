// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/display_hdr.h"

using Common::DisplayHdrInfo;
using Common::ResolveHdrOutput;

TEST_CASE("HDR output follows the monitor's HDR state", "[display_hdr]") {
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const DisplayHdrInfo sdr{
        .hdr_enabled = false, .max_luminance = 270.0f, .sdr_white_level = 80.0f};
    REQUIRE(ResolveHdrOutput(hdr, true, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(sdr, true, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(hdr, false, 0, 0).enabled);
    // Without a host query the setting alone decides.
    REQUIRE(ResolveHdrOutput(std::nullopt, true, 0, 0).enabled);
}

TEST_CASE("HDR luminance of 0 follows the monitor", "[display_hdr]") {
    const DisplayHdrInfo display{
        .hdr_enabled = true, .max_luminance = 1000.4f, .sdr_white_level = 240.0f};
    const auto output = ResolveHdrOutput(display, true, 0, 0);
    REQUIRE(output.peak_luminance == 1000);
    REQUIRE(output.reference_white == 240);
}

TEST_CASE("HDR luminance settings override the monitor", "[display_hdr]") {
    const DisplayHdrInfo display{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const auto output = ResolveHdrOutput(display, true, 600, 900);
    REQUIRE(output.peak_luminance == 600);
    REQUIRE(output.reference_white == 600); // capped at peak
}

TEST_CASE("HDR luminance without a monitor uses the Switch 2 calibration", "[display_hdr]") {
    const auto output = ResolveHdrOutput(std::nullopt, true, 0, 0);
    REQUIRE(output.peak_luminance == 300);
    REQUIRE(output.reference_white == 200);
    REQUIRE(ResolveHdrOutput(std::nullopt, true, 30, 0).peak_luminance == 80);
    REQUIRE(ResolveHdrOutput(std::nullopt, true, 1000, 0).reference_white == 300);
}
