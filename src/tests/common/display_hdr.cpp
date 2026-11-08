// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/display_hdr.h"

using Common::DisplayHdrInfo;
using Common::ResolveHdrOutput;
using Settings::HdrMode;

TEST_CASE("HDR Auto follows the monitor's HDR state", "[display_hdr]") {
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const DisplayHdrInfo sdr{
        .hdr_enabled = false, .max_luminance = 270.0f, .sdr_white_level = 80.0f};
    REQUIRE(ResolveHdrOutput(hdr, HdrMode::Auto, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(sdr, HdrMode::Auto, 0, 0).enabled);
    // A host that cannot tell gets SDR.
    REQUIRE_FALSE(ResolveHdrOutput(std::nullopt, HdrMode::Auto, 0, 0).enabled);
}

TEST_CASE("HDR On and Off ignore the monitor", "[display_hdr]") {
    const DisplayHdrInfo hdr{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const DisplayHdrInfo sdr{
        .hdr_enabled = false, .max_luminance = 270.0f, .sdr_white_level = 80.0f};
    REQUIRE(ResolveHdrOutput(sdr, HdrMode::On, 0, 0).enabled);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::On, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(hdr, HdrMode::Off, 0, 0).enabled);
    REQUIRE_FALSE(ResolveHdrOutput(std::nullopt, HdrMode::Off, 0, 0).enabled);
}

TEST_CASE("HDR luminance of 0 follows the monitor", "[display_hdr]") {
    const DisplayHdrInfo display{
        .hdr_enabled = true, .max_luminance = 1000.4f, .sdr_white_level = 240.0f};
    const auto output = ResolveHdrOutput(display, HdrMode::Auto, 0, 0);
    REQUIRE(output.peak_luminance == 1000);
    REQUIRE(output.reference_white == 240);
}

TEST_CASE("HDR luminance settings override the monitor", "[display_hdr]") {
    const DisplayHdrInfo display{
        .hdr_enabled = true, .max_luminance = 1000.0f, .sdr_white_level = 240.0f};
    const auto output = ResolveHdrOutput(display, HdrMode::Auto, 600, 900);
    REQUIRE(output.peak_luminance == 600);
    REQUIRE(output.reference_white == 600); // capped at peak
}

TEST_CASE("HDR luminance without a monitor uses the Switch 2 calibration", "[display_hdr]") {
    const auto output = ResolveHdrOutput(std::nullopt, HdrMode::Auto, 0, 0);
    REQUIRE(output.peak_luminance == 300);
    REQUIRE(output.reference_white == 200);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::Auto, 30, 0).peak_luminance == 80);
    REQUIRE(ResolveHdrOutput(std::nullopt, HdrMode::Auto, 1000, 0).reference_white == 300);
}
