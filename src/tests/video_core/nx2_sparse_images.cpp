// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"
#include "video_core/vulkan_common/sparse_image_policy.h"

TEST_CASE("Sparse images require backing on every AMD driver by default", "[nx2_sparse_image]") {
    for (const auto driver : {VK_DRIVER_ID_AMD_PROPRIETARY, VK_DRIVER_ID_AMD_OPEN_SOURCE,
                              VK_DRIVER_ID_MESA_RADV}) {
        CHECK_FALSE(Vulkan::CanUseUnbackedSparseImages(driver));
        CHECK_FALSE(Vulkan::CanUseUnbackedSparseImages(driver, '0'));
        CHECK(Vulkan::CanUseUnbackedSparseImages(driver, '1'));
        CHECK_FALSE(Vulkan::CanUseUnbackedSparseImages(driver, 'x'));
    }
    CHECK(Vulkan::CanUseUnbackedSparseImages(VK_DRIVER_ID_NVIDIA_PROPRIETARY));
    CHECK_FALSE(Vulkan::CanUseUnbackedSparseImages(VK_DRIVER_ID_NVIDIA_PROPRIETARY, '0'));
}

TEST_CASE("Sparse image data ranges clip and coalesce written blocks", "[nx2_sparse_image]") {
    Tegra::Nx2ClearSparseRegions();
    constexpr u64 base = 0x1000000;
    constexpr u64 tile = Tegra::Nx2SparseMappingPageSize;
    Tegra::Nx2SparseRegionCreated(base, 6 * tile, 2 * tile);
    Tegra::Nx2SparseBlockWritten(base, 0);
    Tegra::Nx2SparseBlockWritten(base, 1);
    CHECK(Tegra::Nx2SparseDataRanges(base + tile, 5 * tile) ==
          std::vector<std::pair<u64, u64>>{{0, 3 * tile}});
    CHECK(Tegra::Nx2SparseDataRanges(base - 1, tile).empty());
    CHECK(Tegra::Nx2SparseDataRanges(base, 0).empty());
    CHECK(Tegra::Nx2SparseDataRanges(base, std::numeric_limits<u64>::max()).empty());
    Tegra::Nx2ClearSparseRegions();
}

TEST_CASE("Sparse image tile mappings replace coarse written-block ranges", "[nx2_sparse_image]") {
    Tegra::Nx2ClearSparseRegions();
    constexpr u64 base = 0x1000000;
    constexpr u64 tile = Tegra::Nx2SparseMappingPageSize;
    Tegra::Nx2SparseRegionCreated(base, 8 * tile, 4 * tile);
    Tegra::Nx2SparseBlockWritten(base, 0);
    Tegra::Nx2SparseTilesMapped(base, 2 * tile, 2 * tile, true);
    Tegra::Nx2SparseTilesMapped(base, 6 * tile, tile, true);
    CHECK(Tegra::Nx2SparseDataRanges(base, 8 * tile) ==
          std::vector<std::pair<u64, u64>>{{2 * tile, 2 * tile}, {6 * tile, tile}});
    Tegra::Nx2SparseTilesMapped(base, 3 * tile, tile, false);
    CHECK(Tegra::Nx2SparseDataRanges(base + 2 * tile + 16, 5 * tile) ==
          std::vector<std::pair<u64, u64>>{{0, tile - 16}, {4 * tile - 16, tile}});
    Tegra::Nx2SparseRegionDestroyed(base);
    CHECK(Tegra::Nx2SparseDataRanges(base, tile).empty());
}

TEST_CASE("Invalid sparse tile updates cannot discard coarse residency", "[nx2_sparse_image]") {
    Tegra::Nx2ClearSparseRegions();
    constexpr u64 base = 0x1000000;
    constexpr u64 tile = Tegra::Nx2SparseMappingPageSize;
    Tegra::Nx2SparseRegionCreated(base, 4 * tile, 4 * tile);
    Tegra::Nx2SparseBlockWritten(base, 0);
    Tegra::Nx2SparseTilesMapped(base, 4 * tile, tile, true);
    Tegra::Nx2SparseTilesMapped(base, 0, std::numeric_limits<u64>::max(), true);
    CHECK(Tegra::Nx2SparseDataRanges(base, 4 * tile) ==
          std::vector<std::pair<u64, u64>>{{0, 4 * tile}});
    Tegra::Nx2ClearSparseRegions();
}

TEST_CASE("Sparse data range ends cannot overflow a large backing block", "[nx2_sparse_image]") {
    Tegra::Nx2ClearSparseRegions();
    constexpr u64 block = 1ULL << 63;
    Tegra::Nx2SparseRegionCreated(0, std::numeric_limits<u64>::max(), block);
    Tegra::Nx2SparseBlockWritten(0, 1);
    CHECK(Tegra::Nx2SparseDataRanges(block, block - 1) ==
          std::vector<std::pair<u64, u64>>{{0, block - 1}});
    Tegra::Nx2ClearSparseRegions();
}

TEST_CASE("Sparse data ranges clip a partially mapped final tile", "[nx2_sparse_image]") {
    Tegra::Nx2ClearSparseRegions();
    constexpr u64 tile = Tegra::Nx2SparseMappingPageSize;
    Tegra::Nx2SparseRegionCreated(tile, tile + 0x800, tile);
    Tegra::Nx2SparseTilesMapped(tile, tile, 0x800, true);
    CHECK(Tegra::Nx2SparseDataRanges(tile, tile + 0x800) ==
          std::vector<std::pair<u64, u64>>{{tile, 0x800}});
    Tegra::Nx2SparseTilesMapped(tile, tile, 0x800, false);
    CHECK(Tegra::Nx2SparseDataRanges(tile, tile + 0x800).empty());
    Tegra::Nx2ClearSparseRegions();
}

TEST_CASE("Unmapping one guest page preserves neighboring sparse coverage", "[nx2_sparse_image]") {
    Tegra::Nx2ClearSparseRegions();
    constexpr u64 base = 0x1000000;
    Tegra::Nx2SparseRegionCreated(base, 0x10000, 0x10000);
    Tegra::Nx2SparseTilesMapped(base, 0, 0x1000, true);
    Tegra::Nx2SparseTilesMapped(base, 0x1000, 0x1000, true);
    CHECK(Tegra::Nx2SparseDataRanges(base, 0x10000) ==
          std::vector<std::pair<u64, u64>>{{0, 0x2000}});
    Tegra::Nx2SparseTilesMapped(base, 0, 0x1000, false);
    CHECK(Tegra::Nx2SparseDataRanges(base, 0x10000) ==
          std::vector<std::pair<u64, u64>>{{0x1000, 0x1000}});
    Tegra::Nx2ClearSparseRegions();
}
