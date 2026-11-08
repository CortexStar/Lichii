// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/zstd_compression.h"
#include "core/oot_ultrawide.h"

namespace {

void Put32(std::vector<u8>& out, u32 value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(value >> (8 * i)));
    }
}

u32 Get32(const std::vector<u8>& data, size_t offset) {
    u32 value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

// A BYML v7 table like the game's: {Heaps: {DynamicTextureAllocatorMem: {Size: dta},
// Graphics: {Size: gfx}}}. Keys (sorted): 0 DynamicTextureAllocatorMem, 1 Graphics, 2 Heaps, 3 Size.
std::vector<u8> HeapDef(s32 dta, s32 gfx) {
    constexpr std::string_view keys[]{"DynamicTextureAllocatorMem", "Graphics", "Heaps", "Size"};
    std::vector<u8> out{'Y', 'B', 7, 0};
    const u32 key_table = 16;
    Put32(out, key_table);
    Put32(out, 0); // no string table
    const size_t root_slot = out.size();
    Put32(out, 0);
    // Key table: {0xC2, count}, offsets from the table, then the strings.
    Put32(out, 0xC2 | (4u << 8));
    u32 at = 4 + 4 * 4 + 4;
    for (const auto key : keys) {
        Put32(out, at);
        at += static_cast<u32>(key.size()) + 1;
    }
    Put32(out, at); // end offset
    for (const auto key : keys) {
        out.insert(out.end(), key.begin(), key.end());
        out.push_back(0);
    }
    while (out.size() % 4 != 0) {
        out.push_back(0);
    }
    const auto size_node = [&](s32 size) {
        const u32 node = static_cast<u32>(out.size());
        Put32(out, 0xC1 | (1u << 8));
        Put32(out, 3 | (0xD1u << 24));
        Put32(out, static_cast<u32>(size));
        return node;
    };
    const u32 dta_node = size_node(dta);
    const u32 gfx_node = size_node(gfx);
    const u32 heaps = static_cast<u32>(out.size());
    Put32(out, 0xC1 | (2u << 8));
    Put32(out, 0 | (0xC1u << 24));
    Put32(out, dta_node);
    Put32(out, 1 | (0xC1u << 24));
    Put32(out, gfx_node);
    const u32 root = static_cast<u32>(out.size());
    Put32(out, 0xC1 | (1u << 8));
    Put32(out, 2 | (0xC1u << 24));
    Put32(out, heaps);
    std::memcpy(out.data() + root_slot, &root, sizeof(root));
    return out;
}

std::vector<u8> Compress(const std::vector<u8>& data) {
    return Common::Compression::CompressDataZSTDDefault(data.data(), data.size());
}

} // Anonymous namespace

TEST_CASE("Ocarina's HeapDef grows its render-target heap and its parent", "[oot_ultrawide]") {
    using namespace Core::OotUltrawide;
    const auto original = HeapDef(312320, 861184);
    const auto grown = GrowHeapDef(Compress(original), TextureHeapExtraKiB);
    REQUIRE(grown.has_value());
    const auto table = Common::Compression::DecompressDataZSTD(*grown);
    REQUIRE(table.size() == original.size());
    // The header (16 bytes), the key table (24 bytes of offsets, 47 of strings, padded to 88), then
    // the two Size nodes of 12 bytes each, their values 8 bytes in.
    const u32 dta_node = 88;
    REQUIRE(Get32(table, dta_node + 8) == 312320 + 107520);
    REQUIRE(Get32(table, dta_node + 12 + 8) == 861184 + 107520);
    // Everything else is unchanged.
    auto expected = original;
    const u32 dta = 312320 + 107520, gfx = 861184 + 107520;
    std::memcpy(expected.data() + dta_node + 8, &dta, 4);
    std::memcpy(expected.data() + dta_node + 20, &gfx, 4);
    REQUIRE(table == expected);
}

TEST_CASE("The HeapDef edit leaves other data alone", "[oot_ultrawide]") {
    using namespace Core::OotUltrawide;
    const std::vector<u8> text{'n', 'o', 't', ' ', 'b', 'y', 'm', 'l', 0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE_FALSE(GrowHeapDef(Compress(text), TextureHeapExtraKiB).has_value());
    REQUIRE_FALSE(GrowHeapDef(text, TextureHeapExtraKiB).has_value());
    // A heap the game does not use (size 0 or the rest, -1) is not grown.
    REQUIRE_FALSE(GrowHeapDef(Compress(HeapDef(-1, 861184)), TextureHeapExtraKiB).has_value());
}

TEST_CASE("The 3440x1440 heap growth is the shipped heap's share of the extra pixels",
          "[oot_ultrawide]") {
    using namespace Core::OotUltrawide;
    // 312320 KiB * (3440 * 1440 / (2560 * 1440) - 1) = 107360 KiB, rounded up to whole MiB.
    constexpr u64 exact = u64{ShippedTextureHeapKiB} * (3440 - 2560) / 2560;
    STATIC_REQUIRE(exact == 107360);
    STATIC_REQUIRE(TextureHeapExtraKiB == (exact + 1023) / 1024 * 1024);
    STATIC_REQUIRE(MemoryExtraBytes == ((u64{TextureHeapExtraKiB} << 10) + 0x1FFFFF) / 0x200000 *
                                           0x200000);
}
