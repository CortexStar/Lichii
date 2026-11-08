// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/nx2_write_handoff.h"

namespace {
using Runs = std::vector<std::pair<u64, u64>>;

Runs Drain(VideoCommon::Nx2WriteHandoff& handoff) {
    Runs runs;
    handoff.Drain([&](u64 address, u64 size) { runs.emplace_back(address, size); });
    return runs;
}
} // namespace

TEST_CASE("Write handoff hands out queued writes as 64-byte granule runs",
          "[video_core][nx2_write_handoff]") {
    VideoCommon::Nx2WriteHandoff handoff;
    REQUIRE(Drain(handoff).empty());
    REQUIRE(handoff.Push(0x10010, 4));
    REQUIRE(Drain(handoff) == Runs{{0x10000, 0x40}});
    REQUIRE(Drain(handoff).empty());
}

TEST_CASE("Write handoff merges adjacent granules and pages in address order",
          "[video_core][nx2_write_handoff]") {
    VideoCommon::Nx2WriteHandoff handoff;
    REQUIRE(handoff.Push(0x20800, 0x40)); // second 2 KiB page, first granule
    REQUIRE(handoff.Push(0x207c0, 0x40)); // first page, last granule: touches the one above
    REQUIRE(handoff.Push(0x30000, 0x80));
    REQUIRE(handoff.Push(0x30040, 0x10)); // already queued
    REQUIRE(Drain(handoff) == Runs{{0x207c0, 0x80}, {0x30000, 0x80}});
}

TEST_CASE("Write handoff splits writes across a page boundary", "[video_core][nx2_write_handoff]") {
    VideoCommon::Nx2WriteHandoff handoff;
    REQUIRE(handoff.Push(0x407f0, 0x20));
    REQUIRE(Drain(handoff) == Runs{{0x407c0, 0x80}});
}

TEST_CASE("Write handoff refuses what it cannot queue", "[video_core][nx2_write_handoff]") {
    VideoCommon::Nx2WriteHandoff handoff;
    // More than two pages.
    REQUIRE_FALSE(handoff.Push(0x50000, 0x1001));
    // Out of slots: 32 pages fit, the 33rd does not, and what fit is still handed out.
    for (u64 page = 0; page < 32; ++page) {
        REQUIRE(handoff.Push(0x100000 + page * 0x1000, 4));
    }
    REQUIRE_FALSE(handoff.Push(0x200000, 4));
    REQUIRE(Drain(handoff).size() == 32);
    REQUIRE(handoff.Push(0x200000, 4));
    REQUIRE(Drain(handoff) == Runs{{0x200000, 0x40}});
}
