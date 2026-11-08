// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

TEST_CASE("Nx2MayBeGpuWritten sees every page a GPU write marked", "[video_core][nx2]") {
    constexpr u64 page = u64{1} << Tegra::Nx2WrittenPageBits;
    constexpr u64 base = 0x10000000;
    Tegra::Nx2ClearGpuWrittenPages();
    CHECK_FALSE(Tegra::Nx2MayBeGpuWritten(base, 64 * page));

    Tegra::Nx2MarkGpuWritten(base + 3 * page + 100, 8);
    CHECK(Tegra::Nx2MayBeGpuWritten(base + 3 * page, 1));
    CHECK(Tegra::Nx2MayBeGpuWritten(base + 4 * page - 1, 1));
    CHECK(Tegra::Nx2MayBeGpuWritten(base, 3 * page + 1));
    CHECK(Tegra::Nx2MayBeGpuWritten(base + 3 * page - 4, 8));
    CHECK(Tegra::Nx2MayBeGpuWritten(base, 64 * page));
    CHECK_FALSE(Tegra::Nx2MayBeGpuWritten(base, 3 * page));
    CHECK_FALSE(Tegra::Nx2MayBeGpuWritten(base + 4 * page, 60 * page));
    CHECK_FALSE(Tegra::Nx2MayBeGpuWritten(base + 3 * page, 0));

    Tegra::Nx2MarkGpuWritten(base + 63 * page + page - 1, 2);
    CHECK(Tegra::Nx2MayBeGpuWritten(base + 64 * page, 1));
    CHECK_FALSE(Tegra::Nx2MayBeGpuWritten(base + 65 * page, page));

    const u64 bitmap_end = Tegra::g_nx2_gpu_written_pages.size() * 64 * page;
    CHECK(Tegra::Nx2MayBeGpuWritten(bitmap_end - 4, 8));
    CHECK(Tegra::Nx2MayBeGpuWritten(bitmap_end, page));
    CHECK(Tegra::Nx2MayBeGpuWritten(~u64{0} - 3, 8));

    Tegra::Nx2ClearGpuWrittenPages();
    CHECK_FALSE(Tegra::Nx2MayBeGpuWritten(base, 64 * page));
}

TEST_CASE("Nx2MarkGpuWritten marks a range over several bitmap words exactly",
          "[video_core][nx2]") {
    constexpr u64 page = u64{1} << Tegra::Nx2WrittenPageBits;
    constexpr u64 base = 0x20000000; // Page-aligned at a bitmap word
    Tegra::Nx2ClearGpuWrittenPages();
    // Pages 5 through 134: the end of one word, a full word and the start of the next.
    Tegra::Nx2MarkGpuWritten(base + 5 * page + 1, 130 * page - 2);
    CHECK_FALSE(Tegra::Nx2IsGpuWritten(base + 4 * page));
    for (u64 index = 5; index <= 134; ++index) {
        CHECK(Tegra::Nx2IsGpuWritten(base + index * page));
    }
    CHECK_FALSE(Tegra::Nx2IsGpuWritten(base + 135 * page));
    // Marking again changes nothing.
    Tegra::Nx2MarkGpuWritten(base + 5 * page, page);
    CHECK_FALSE(Tegra::Nx2IsGpuWritten(base + 4 * page));
    Tegra::Nx2ClearGpuWrittenPages();
}
