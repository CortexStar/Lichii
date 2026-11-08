// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/vulkan_common/address_bindings.h"

namespace {
using Vulkan::AddressBindings::History;

bool Has(const std::vector<std::string>& lines, std::string_view part) {
    for (const std::string& line : lines) {
        if (line.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}
} // Anonymous namespace

TEST_CASE("Address bindings: a fault inside a live buffer names it and the offset",
          "[video_core][diagnostics]") {
    History history;
    history.Bind(0x100000, 0x1000, VK_OBJECT_TYPE_BUFFER, 0xb1, "vmaCreateBuffer( size=0x1000 )");
    history.Bind(0x200000, 0x2000, VK_OBJECT_TYPE_IMAGE, 0x11, "vmaCreateImage( 2D 64x64x1 )");
    const auto lines = history.Explain(0x100840, 0);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] ==
          "in BUFFER 0xb1 [0x100000, 0x101000) at +0x840: vmaCreateBuffer( size=0x1000 )");
    // A reported address with a precision: the range it rounds down to.
    CHECK(Has(history.Explain(0x200fff, 0x1000), "in IMAGE 0x11 [0x200000, 0x202000) at +0x0"));
}

TEST_CASE("Address bindings: 0x20 bytes past an end names the neighbour",
          "[video_core][diagnostics]") {
    History history;
    history.Bind(0x100000, 0x1000, VK_OBJECT_TYPE_BUFFER, 0xb1, "page table");
    history.Bind(0x108000, 0x1000, VK_OBJECT_TYPE_BUFFER, 0xb2, "next");
    const auto lines = history.Explain(0x101020, 0);
    CHECK(lines[0] == "no object bound here");
    CHECK(Has(lines, "0x20 bytes past the end of BUFFER 0xb1 [0x100000, 0x101000): page table"));
    CHECK(Has(lines, "0x6fe0 bytes before BUFFER 0xb2"));
}

TEST_CASE("Address bindings: a freed range is a use after free", "[video_core][diagnostics]") {
    History history;
    history.Bind(0x300000, 0x1000, VK_OBJECT_TYPE_BUFFER, 0xb3, "staging");
    history.Bind(0x400000, 0x1000, VK_OBJECT_TYPE_BUFFER, 0xb4, "other");
    history.Unbind(0x300000, 0x1000, VK_OBJECT_TYPE_BUFFER, 0xb3);
    CHECK(history.LiveCount() == 1);
    const auto lines = history.Explain(0x300010, 0);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "in freed BUFFER 0xb3 [0x300000, 0x301000) at +0x10 (0 binding changes "
                      "since it was freed): staging");
}

TEST_CASE("Address bindings: null addresses and an empty history", "[video_core][diagnostics]") {
    History history;
    CHECK(history.Explain(0x200000, 0).front() == "no object: nothing bound (0 live ranges)");
    history.Bind(0x100000, 0x1000, VK_OBJECT_TYPE_BUFFER, 0xb1, "x");
    const auto null_lines = history.Explain(0x0, 0x1000);
    REQUIRE(null_lines.size() == 1);
    CHECK(null_lines[0].starts_with("null address"));
}

TEST_CASE("Address bindings: the freed list keeps the newest", "[video_core][diagnostics]") {
    History history{2};
    for (u64 i = 0; i < 3; ++i) {
        history.Bind(0x100000 * (i + 1), 0x100, VK_OBJECT_TYPE_BUFFER, i, "b");
        history.Unbind(0x100000 * (i + 1), 0x100, VK_OBJECT_TYPE_BUFFER, i);
    }
    CHECK(!Has(history.Explain(0x100000, 0), "freed"));
    CHECK(Has(history.Explain(0x300000, 0), "in freed BUFFER 0x2"));
}
