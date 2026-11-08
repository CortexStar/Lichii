// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nso.h"

namespace {

Loader::NSOHeader MakeHeader(u32 data_location, u32 data_size, u32 bss_size) {
    Loader::NSOHeader header{};
    header.segments[0].location = 0;
    header.segments[0].size = 0x3000;
    header.segments[1].location = 0x3000;
    header.segments[1].size = 0x1000;
    header.segments[2].location = data_location;
    header.segments[2].size = data_size;
    header.segments[2].bss_size = bss_size;
    return header;
}

} // Anonymous namespace

TEST_CASE("NSO image size covers the segments and .bss, page aligned", "[core][loader][nso]") {
    REQUIRE(Loader::AppLoader_NSO::ImageSize(MakeHeader(0x4000, 0x1000, 0), false) == 0x5000);
    REQUIRE(Loader::AppLoader_NSO::ImageSize(MakeHeader(0x4000, 0x800, 0), false) == 0x5000);
    REQUIRE(Loader::AppLoader_NSO::ImageSize(MakeHeader(0x4000, 0x800, 0x1801), false) == 0x7000);
}

TEST_CASE("NSO image size adds the argument area before .bss", "[core][loader][nso]") {
    const auto header = MakeHeader(0x4000, 0x1000, 0x100);
    REQUIRE(Loader::AppLoader_NSO::ImageSize(header, true) ==
            0x5000 + Loader::NSO_ARGUMENT_DATA_ALLOCATION_SIZE + 0x1000);
}
