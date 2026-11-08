// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "video_core/oot_network.h"

using namespace VideoCommon::OotNetwork;

TEST_CASE("Ocarina of Time tonemap shaders are recognized", "[video_core][oot_network]") {
    STATIC_REQUIRE(IsTonemapShader(0xbded5ce917183214ULL));
    STATIC_REQUIRE(IsTonemapShader(0x6cb06edf14be1d79ULL));
    STATIC_REQUIRE(IsTonemapShader(0x3ac011bd2d772994ULL));
    STATIC_REQUIRE_FALSE(IsTonemapShader(0));
    STATIC_REQUIRE_FALSE(IsTonemapShader(PrepassProgramKey));
}

TEST_CASE("Ocarina of Time network bypass follows the setting and the game",
          "[video_core][oot_network]") {
    auto& setting = Settings::values.oot_anti_aliasing;
    const auto previous = setting.GetValue();

    setting.SetValue(Settings::OotAntiAliasing::Game);
    CHECK_FALSE(BypassEnabled(ProgramId));

    setting.SetValue(Settings::OotAntiAliasing::NoSharpening);
    CHECK(BypassEnabled(ProgramId));
    CHECK_FALSE(BypassEnabled(0x0400C3F00006E000ULL));

    setting.SetValue(previous);
}

TEST_CASE("Ocarina of Time network passes that only feed the output pass are skipped with it",
          "[video_core][oot_network]") {
    constexpr u64 OtherKey = 0x1234;
    constexpr u64 OtherHash = 0x5678;
    CHECK(FeedsOutputPass(PrepassProgramKey, OtherHash));
    CHECK(FeedsOutputPass(NetworkProgramKey, OtherHash));
    // The heavy dispatch is known by its shader hash.
    CHECK(FeedsOutputPass(OtherKey, HeavyDispatchShaderHash));
    // The output pass itself is replaced, not skipped; other passes run.
    CHECK_FALSE(FeedsOutputPass(OutputProgramKey, OtherHash));
    CHECK_FALSE(FeedsOutputPass(OtherKey, OtherHash));
}
