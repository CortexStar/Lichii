// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/nx2_video_decode_policy.h"

TEST_CASE("NX2 CPU AV1 decode threads scale with the stream", "[video_core][nx2_video]") {
    using Tegra::Host1x::Nx2SoftwareDecodeThreads;
    // Ocarina of Time's movies (1920x1080 at 60) on 16 and 8 logical CPUs: two threads.
    REQUIRE(Nx2SoftwareDecodeThreads(1920, 1080, 60, 16) == 2);
    REQUIRE(Nx2SoftwareDecodeThreads(1920, 1080, 60, 8) == 2);
    // Rate not given: taken as 60.
    REQUIRE(Nx2SoftwareDecodeThreads(1920, 1080, 0, 16) == 2);
    // Smaller or slower streams never go below two.
    REQUIRE(Nx2SoftwareDecodeThreads(1280, 720, 30, 16) == 2);
    // 1440p60 is 1.8 units, 4K60 four.
    REQUIRE(Nx2SoftwareDecodeThreads(2560, 1440, 60, 16) == 4);
    REQUIRE(Nx2SoftwareDecodeThreads(3840, 2160, 60, 16) == 8);
    // Never more than half the logical CPUs (but at least two).
    REQUIRE(Nx2SoftwareDecodeThreads(3840, 2160, 60, 8) == 4);
    REQUIRE(Nx2SoftwareDecodeThreads(3840, 2160, 60, 2) == 2);
}

TEST_CASE("NX2 GPU decoder failure moves the stream on", "[video_core][nx2_video]") {
    using Tegra::Host1x::Nx2GpuDecoderFailed;
    using Tegra::Host1x::Nx2MaxPacketsWithoutPicture;
    // Any error, before or after the first picture (the ENOMEM of FFmpeg 9.0.2's Vulkan Video
    // decoder on AMD came at the first packet).
    REQUIRE(Nx2GpuDecoderFailed(true, false, 1));
    REQUIRE(Nx2GpuDecoderFailed(true, true, 0));
    // A decoder that takes packets without ever producing a picture.
    REQUIRE_FALSE(Nx2GpuDecoderFailed(false, false, Nx2MaxPacketsWithoutPicture));
    REQUIRE(Nx2GpuDecoderFailed(false, false, Nx2MaxPacketsWithoutPicture + 1));
    // Once it produced pictures, a gap is the stream's (hidden frames), not a failure.
    REQUIRE_FALSE(Nx2GpuDecoderFailed(false, true, 1000));
}

TEST_CASE("NX2 AV1 packets that start a sequence", "[video_core][nx2_video]") {
    using Tegra::Host1x::Nx2Av1StartsSequence;
    // Temporal delimiter (type 2, size 0), sequence header (type 1, size 2), frame (type 6).
    constexpr std::array<u8, 9> key{0x12, 0x00, 0x0a, 0x02, 0xaa, 0xbb, 0x32, 0x01, 0xcc};
    REQUIRE(Nx2Av1StartsSequence(key));
    // Temporal delimiter and a frame only.
    constexpr std::array<u8, 5> inter{0x12, 0x00, 0x32, 0x01, 0xcc};
    REQUIRE_FALSE(Nx2Av1StartsSequence(inter));
    // An extension byte before the size, and a two-byte LEB128 size (130).
    std::array<u8, 136> extended{};
    extended[0] = 0x36; // frame, extension, has size
    extended[1] = 0x00;
    extended[2] = 0x82;
    extended[3] = 0x01;
    extended[134] = 0x0a; // sequence header with size 0 after the 130-byte frame
    extended[135] = 0x00;
    REQUIRE(Nx2Av1StartsSequence(extended));
    // Truncated sizes and the forbidden bit stop the walk.
    constexpr std::array<u8, 3> truncated{0x32, 0x05, 0x00};
    REQUIRE_FALSE(Nx2Av1StartsSequence(truncated));
    constexpr std::array<u8, 2> forbidden{0x8a, 0x00};
    REQUIRE_FALSE(Nx2Av1StartsSequence(forbidden));
    REQUIRE_FALSE(Nx2Av1StartsSequence({}));
}
