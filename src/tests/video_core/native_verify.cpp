// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/native_verify.h"

using VideoCommon::NativeVerify::Layer;
using VideoCommon::NativeVerify::ParseMask;

TEST_CASE("EDEN_NX2_NATIVE_VERIFY names layers by name, comma separated", "[native_verify]") {
    constexpr u32 storage = 1U << static_cast<u32>(Layer::Storage);
    constexpr u32 uniforms = 1U << static_cast<u32>(Layer::Uniforms);
    REQUIRE(ParseMask("") == 0);
    REQUIRE(ParseMask("storage") == storage);
    REQUIRE(ParseMask("uniforms,storage") == (storage | uniforms));
    REQUIRE(ParseMask("all") == (storage | uniforms));
    REQUIRE(ParseMask("textures") == 0);
    REQUIRE(ParseMask("storage,") == storage);
}
