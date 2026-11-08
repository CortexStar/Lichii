// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/gpu_page_table.h"

using VideoCommon::GpuPageTable;

namespace {

constexpr u64 NULL_PAGE = 0x7000'0000;

/// What a shader computes from the uploaded table: the same two loads, from the image.
u64 ShaderTranslate(const GpuPageTable& table, u64 va) {
    const auto image = table.Image();
    u32 block{};
    std::memcpy(&block, image.data() + (va >> 21) * sizeof(u32), sizeof(block));
    u64 page{};
    std::memcpy(&page,
                image.data() + GpuPageTable::BlockPoolOffset() + u64{block} * 4096 +
                    ((va >> 12) & 511) * sizeof(u64),
                sizeof(page));
    return page + (va & 4095);
}

/// Applies the table's updates to a device copy, as the renderer's upload does.
void Upload(GpuPageTable& table, std::vector<u8>& device) {
    std::vector<GpuPageTable::Update> updates;
    table.TakeUpdates(updates);
    for (const auto& update : updates) {
        REQUIRE(update.offset + update.size <= device.size());
        std::memcpy(device.data() + update.offset, table.Image().data() + update.offset,
                    update.size);
    }
}

} // namespace

TEST_CASE("GPU page table translates mapped pages and leaves the rest at the null page",
          "[gpu_page_table]") {
    GpuPageTable table{8};
    table.SetNullPage(NULL_PAGE);
    REQUIRE(table.Translate(0x1234'5678) == NULL_PAGE + 0x678);
    REQUIRE_FALSE(table.IsMapped(0x1234'5678));

    REQUIRE(table.Map(0x10'0000'0000, 0x3000, 0x8000'0000));
    REQUIRE(table.Translate(0x10'0000'0000) == 0x8000'0000);
    REQUIRE(table.Translate(0x10'0000'2ffc) == 0x8000'2ffc);
    REQUIRE(table.Translate(0x10'0000'3000) == NULL_PAGE);
    REQUIRE(ShaderTranslate(table, 0x10'0000'1abc) == 0x8000'1abc);
    REQUIRE(table.UsedBlocks() == 2);
}

TEST_CASE("GPU page table maps unaligned ranges by whole pages, across L2 blocks",
          "[gpu_page_table]") {
    GpuPageTable table{8};
    table.SetNullPage(NULL_PAGE);
    // 0x1ff800..0x201800 crosses the 2 MiB boundary: pages 0x1ff and 0x200, 0x201.
    REQUIRE(table.Map(0x1ff800, 0x2000, 0x5'0000'0800));
    REQUIRE(table.Translate(0x1ff800) == 0x5'0000'0800);
    REQUIRE(table.Translate(0x1ff000) == 0x5'0000'0000);
    REQUIRE(table.Translate(0x200000) == 0x5'0000'1000);
    REQUIRE(table.Translate(0x2017ff) == 0x5'0000'27ff);
    REQUIRE(table.Translate(0x202000) == NULL_PAGE);
    REQUIRE(table.UsedBlocks() == 3);
}

TEST_CASE("GPU page table frees empty blocks and reuses them", "[gpu_page_table]") {
    GpuPageTable table{3};
    table.SetNullPage(NULL_PAGE);
    REQUIRE(table.Map(0x0, 0x1000, 0x1000'0000));
    REQUIRE(table.Map(0x400000, 0x1000, 0x2000'0000));
    // Out of blocks: the null block and two others.
    REQUIRE_FALSE(table.Map(0x800000, 0x1000, 0x3000'0000));
    table.Unmap(0x0, 0x1000);
    REQUIRE(table.Translate(0x0) == NULL_PAGE);
    REQUIRE(table.Map(0x800000, 0x1000, 0x3000'0000));
    REQUIRE(table.Translate(0x800000) == 0x3000'0000);
    REQUIRE(table.Translate(0x400000) == 0x2000'0000);
}

TEST_CASE("GPU page table updates reproduce the image on the device", "[gpu_page_table]") {
    GpuPageTable table{16};
    std::vector<u8> device(table.TotalBytes(), 0xcd);
    table.SetNullPage(NULL_PAGE);
    Upload(table, device);
    REQUIRE_FALSE(table.HasUpdates());
    REQUIRE(table.Map(0x3'0000'0000, 0x20'0000, 0x9000'0000));
    REQUIRE(table.Map(0x7'0000'5000, 0x1000, 0xa000'0000));
    table.Unmap(0x3'0000'1000, 0x1000);
    REQUIRE(table.HasUpdates());
    Upload(table, device);
    // Every byte the shader can reach equals the image: L1 and the blocks in use.
    REQUIRE(std::memcmp(device.data(), table.Image().data(),
                        GpuPageTable::L1_BYTES + table.UsedBlocks() * GpuPageTable::BLOCK_BYTES) ==
            0);
    REQUIRE(ShaderTranslate(table, 0x3'0000'0010) == 0x9000'0010);
    REQUIRE(ShaderTranslate(table, 0x3'0000'1010) == NULL_PAGE + 0x10);
    REQUIRE(ShaderTranslate(table, 0x7'0000'5ff0) == 0xa000'0ff0);
}

TEST_CASE("GPU page table drops the pages of a host buffer that goes away", "[gpu_page_table]") {
    GpuPageTable table{8};
    table.SetNullPage(NULL_PAGE);
    REQUIRE(table.Map(0x10000, 0x4000, 0x4000'0000));
    REQUIRE(table.Map(0x90000, 0x1000, 0x4000'2000)); // aliases the third page
    REQUIRE(table.Map(0x20000, 0x1000, 0x5000'0000));
    REQUIRE(table.UnmapDevice(0x4000'2000, 0x4000'3000) == 2);
    REQUIRE(table.Translate(0x12000) == NULL_PAGE);
    REQUIRE(table.Translate(0x90000) == NULL_PAGE);
    REQUIRE(table.Translate(0x13000) == 0x4000'3000);
    REQUIRE(table.Translate(0x20000) == 0x5000'0000);
}

TEST_CASE("GPU page table moves unmapped entries to a new null page", "[gpu_page_table]") {
    GpuPageTable table{8};
    table.SetNullPage(NULL_PAGE);
    REQUIRE(table.Map(0x0, 0x1000, 0x1000'0000));
    table.SetNullPage(0x6000'0000);
    REQUIRE(table.Translate(0x1000) == 0x6000'0000);
    REQUIRE(table.Translate(0x5'0000'0000) == 0x6000'0000);
    REQUIRE(table.Translate(0x0) == 0x1000'0000);
}

TEST_CASE("GPU page table resends its whole image to a new device copy", "[gpu_page_table]") {
    GpuPageTable table{8};
    table.SetNullPage(NULL_PAGE);
    REQUIRE(table.Map(0x4000'0000, 0x1000, 0x2000'0000));
    std::vector<u8> first(table.TotalBytes(), 0);
    Upload(table, first);
    REQUIRE_FALSE(table.HasUpdates());
    // The device copy is recreated: everything a shader reaches must arrive again.
    std::vector<u8> second(table.TotalBytes(), 0xee);
    table.MarkAllDirty();
    Upload(table, second);
    REQUIRE(std::memcmp(second.data(), table.Image().data(),
                        GpuPageTable::L1_BYTES + table.UsedBlocks() * GpuPageTable::BLOCK_BYTES) ==
            0);
    REQUIRE(ShaderTranslate(table, 0x4000'0123) == 0x2000'0123);
}

TEST_CASE("GPU page table maps a range again without walking it while it stands",
          "[gpu_page_table]") {
    GpuPageTable table{64};
    table.SetNullPage(NULL_PAGE);
    std::vector<GpuPageTable::Update> updates;
    REQUIRE(table.Map(0x1000'0000, 0x400'0000, 0x8'0000'0000)); // 64 MiB
    table.TakeUpdates(updates);
    // A binding inside it at the same offset: nothing to write.
    REQUIRE(table.Map(0x1100'0040, 0x10'0000, 0x8'0100'0040));
    REQUIRE_FALSE(table.HasUpdates());
    // The host buffer goes away: the same Map writes its pages again.
    REQUIRE(table.UnmapDevice(0x8'0100'0000, 0x8'0101'0000) == 16);
    REQUIRE(table.Translate(0x1100'0040) == NULL_PAGE + 0x40);
    table.TakeUpdates(updates);
    REQUIRE(table.Map(0x1100'0040, 0x10'0000, 0x8'0100'0040));
    REQUIRE(table.HasUpdates());
    REQUIRE(table.Translate(0x1100'0040) == 0x8'0100'0040);
    // A different device offset over a standing range rewrites it too.
    REQUIRE(table.Map(0x1000'0000, 0x1000, 0x9'0000'0000));
    REQUIRE(table.Translate(0x1000'0010) == 0x9'0000'0010);
    // A guest unmap inside an extent drops it.
    table.Unmap(0x1200'0000, 0x1000);
    REQUIRE(table.Translate(0x1200'0000) == NULL_PAGE);
    REQUIRE(table.Map(0x1200'0000, 0x1000, 0x8'0200'0000));
    REQUIRE(table.Translate(0x1200'0000) == 0x8'0200'0000);
}
