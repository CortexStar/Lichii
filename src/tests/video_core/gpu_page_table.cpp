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

TEST_CASE("GPU page table keeps host addresses that are not page aligned", "[gpu_page_table]") {
    // Host buffers are suballocated at their own alignment: a buffer holding a guest page from
    // its start can begin 0x20 bytes past a host page boundary.
    GpuPageTable table{8};
    constexpr u64 null_page = 0x6000'0040; // a suballocated zero buffer, not page aligned
    table.SetNullPage(null_page);
    REQUIRE(table.Translate(0x5000'0010) == null_page + 0x10);
    REQUIRE(table.Map(0x217f'6b8000, 0x2000, 0x2141'e9020));
    REQUIRE(table.Translate(0x217f'6b8000) == 0x2141'e9020);
    REQUIRE(table.Translate(0x217f'6b8fff) == 0x2141'ea01f);
    REQUIRE(table.Translate(0x217f'6b9000) == 0x2141'ea020);
    REQUIRE(ShaderTranslate(table, 0x217f'6b9300) == 0x2141'ea320);
    REQUIRE(table.IsMapped(0x217f'6b8fff));
    REQUIRE_FALSE(table.IsMapped(0x217f'6ba000));
    // A run that starts inside a page maps its page from the address its first byte has.
    REQUIRE(table.Map(0x217f'4b9300, 0x100, 0x213f'ea320));
    REQUIRE(table.Translate(0x217f'4b9300) == 0x213f'ea320);
    REQUIRE(table.Translate(0x217f'4b9000) == 0x213f'ea020);
    // The fast path for a standing range keeps the offset too.
    REQUIRE(table.Map(0x217f'6b8800, 0x10, 0x2141'e9820));
    REQUIRE(table.Translate(0x217f'6b8800) == 0x2141'e9820);
    // Dropping the buffer finds its unaligned entries.
    REQUIRE(table.UnmapDevice(0x2141'e9020, 0x2141'eb020) == 2);
    REQUIRE(table.Translate(0x217f'6b8fff) == null_page + 0xfff);
}

TEST_CASE("GPU page table finds a host buffer's pages after maps over parts of a range",
          "[gpu_page_table]") {
    GpuPageTable table{3}; // the null block and two more
    table.SetNullPage(NULL_PAGE);
    // Four pages from buffer A, then the second page remapped to buffer B.
    REQUIRE(table.Map(0x10000, 0x4000, 0xa000'0000));
    REQUIRE(table.Map(0x11000, 0x1000, 0xb000'0000));
    REQUIRE(table.Translate(0x11000) == 0xb000'0000);
    REQUIRE(table.Translate(0x12000) == 0xa000'2000);
    // Buffer A goes away: its three remaining pages, not B's.
    REQUIRE(table.UnmapDevice(0xa000'0000, 0xa000'4000) == 3);
    REQUIRE(table.Translate(0x10000) == NULL_PAGE);
    REQUIRE(table.Translate(0x11000) == 0xb000'0000);
    REQUIRE(table.Translate(0x13000) == NULL_PAGE);
    // A guest unmap in the middle of a range leaves both sides findable.
    REQUIRE(table.Map(0x20000, 0x3000, 0xc000'0000));
    table.Unmap(0x21000, 0x1000);
    REQUIRE(table.UnmapDevice(0xc000'2000, 0xc000'3000) == 1);
    REQUIRE(table.Translate(0x20000) == 0xc000'0000);
    REQUIRE(table.Translate(0x22000) == NULL_PAGE);
    // Pages mapped before the blocks ran out are still found.
    REQUIRE_FALSE(table.Map(0x1f'ffe000, 0x4000, 0xd000'0000)); // one block, then none
    REQUIRE(table.Translate(0x1f'ffe000) == 0xd000'0000);
    REQUIRE(table.UnmapDevice(0xd000'0000, 0xd000'4000) == 2);
    REQUIRE(table.Translate(0x1f'fff000) == NULL_PAGE);
}

TEST_CASE("GPU page table tells map changes from unmaps", "[gpu_page_table]") {
    GpuPageTable table{8};
    table.SetNullPage(NULL_PAGE);
    std::vector<GpuPageTable::Update> updates;
    table.TakeUpdates(updates);
    REQUIRE_FALSE(table.HasMapChanges());
    REQUIRE(table.Map(0x10000, 0x2000, 0xa000'0000));
    REQUIRE(table.HasMapChanges());
    table.TakeUpdates(updates);
    REQUIRE_FALSE(table.HasMapChanges());
    // The same mapping again writes nothing.
    REQUIRE(table.Map(0x10000, 0x2000, 0xa000'0000));
    REQUIRE_FALSE(table.HasUpdates());
    // An unmap alone is an update, not a map change.
    REQUIRE(table.UnmapDevice(0xa000'0000, 0xa000'1000) == 1);
    REQUIRE(table.HasUpdates());
    REQUIRE_FALSE(table.HasMapChanges());
    REQUIRE(table.Map(0x10000, 0x1000, 0xb000'0000));
    REQUIRE(table.HasMapChanges());
}
