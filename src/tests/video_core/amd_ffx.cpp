// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/amd_ffx.h"

TEST_CASE("AMD FSR DLL folders: setting, environment, user folder, executable", "[video_core][amd]") {
    using Vulkan::AmdFfx::SearchDirs;
    const std::filesystem::path user{"user/amd-fsr"};
    const std::filesystem::path exe{"bin"};
    const auto all = SearchDirs("chosen", "env", user, exe);
    REQUIRE(all.size() == 4);
    CHECK(all[0] == std::filesystem::path{"chosen"});
    CHECK(all[1] == std::filesystem::path{"env"});
    CHECK(all[2] == user);
    CHECK(all[3] == exe / "amd-fsr");

    const auto defaults = SearchDirs("", nullptr, user, exe);
    REQUIRE(defaults.size() == 2);
    CHECK(defaults[0] == user);
    CHECK(defaults[1] == exe / "amd-fsr");

    CHECK(SearchDirs("", "", user, exe).size() == 2);
}

TEST_CASE("AMD FSR versions: FSR 4 is any 4.x or newer, newest wins", "[video_core][amd]") {
    using Vulkan::AmdFfx::IsVersion4OrNewer;
    using Vulkan::AmdFfx::NewestVersion4;
    CHECK(IsVersion4OrNewer("4.1.2"));
    CHECK(IsVersion4OrNewer("FSR 4.0.2"));
    CHECK(IsVersion4OrNewer("5.0.0"));
    CHECK(IsVersion4OrNewer("10.0"));
    CHECK_FALSE(IsVersion4OrNewer("3.1.6"));
    CHECK_FALSE(IsVersion4OrNewer("2.3.4"));
    CHECK_FALSE(IsVersion4OrNewer("unknown"));

    // The list the runtime reported on the RX 9070 XT with driver 26.10.2.
    const std::vector<std::string> driver{"4.1.2", "3.1.6", "2.3.4"};
    CHECK(NewestVersion4(driver) == 0U);
    const std::vector<std::string> mixed{"3.1.6", "4.0.2", "4.1.10", "4.1.2"};
    CHECK(NewestVersion4(mixed) == 2U);
    const std::vector<std::string> old{"3.1.6", "2.3.4"};
    CHECK_FALSE(NewestVersion4(old).has_value());
}
