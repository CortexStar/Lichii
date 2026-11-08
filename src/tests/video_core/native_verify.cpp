// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/native_verify.h"

using VideoCommon::NativeVerify::Layer;
using VideoCommon::NativeVerify::ParseMask;

TEST_CASE("EDEN_NX2_NATIVE_VERIFY names layers by name, comma separated", "[native_verify]") {
    constexpr u32 storage = 1U << static_cast<u32>(Layer::Storage);
    constexpr u32 uniforms = 1U << static_cast<u32>(Layer::Uniforms);
    constexpr u32 textures = 1U << static_cast<u32>(Layer::Textures);
    constexpr u32 bindless = 1U << static_cast<u32>(Layer::Bindless);
    constexpr u32 barriers = 1U << static_cast<u32>(Layer::Barriers);
    constexpr u32 dynstate = 1U << static_cast<u32>(Layer::DynState);
    constexpr u32 conditions = 1U << static_cast<u32>(Layer::Conditions);
    REQUIRE(ParseMask("") == 0);
    REQUIRE(ParseMask("storage") == storage);
    REQUIRE(ParseMask("uniforms,storage") == (storage | uniforms));
    REQUIRE(ParseMask("all") ==
            (storage | uniforms | textures | bindless | barriers | dynstate | conditions));
    REQUIRE(ParseMask("dynstate") == dynstate);
    REQUIRE(ParseMask("conditions") == conditions);
    REQUIRE(ParseMask("textures") == textures);
    REQUIRE(ParseMask("bindless,textures") == (textures | bindless));
    REQUIRE(ParseMask("texture") == 0);
    REQUIRE(ParseMask("storage,") == storage);
    REQUIRE(ParseMask(",textures") == textures);
}
