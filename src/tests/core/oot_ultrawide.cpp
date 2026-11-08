// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>
#include <utility>
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

namespace {

void Put64(std::vector<u8>& out, size_t at, u64 value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}

using Panes = std::vector<std::pair<std::string_view, f32>>;

// A "BFFH" layout like the game's: section header at 0x20, "UIL" data at 0x40, then the name and
// record tables, the 68-byte pane records from 0x100 and the names after them.
std::vector<u8> Layout(const Panes& panes) {
    const u64 names = 0x80, records = 0xC0, first_record = 0x100;
    const u64 strings = first_record + 68 * panes.size();
    std::vector<u8> out(strings, 0);
    std::memcpy(out.data(), "BFFH", 4);
    const u32 section = 0x20, base = 0x40;
    std::memcpy(out.data() + 0x10, &section, 4);
    std::memcpy(out.data() + section, "BFSI", 4);
    std::memcpy(out.data() + section + 4, &base, 4);
    std::memcpy(out.data() + base, "UIL", 3);
    Put64(out, base + 0x20, names);
    Put64(out, base + 0x28, records);
    const u32 count = static_cast<u32>(panes.size());
    std::memcpy(out.data() + base + 0x38, &count, 4);
    for (size_t i = 0; i < panes.size(); ++i) {
        Put64(out, names + 8 * i, out.size());
        out.insert(out.end(), panes[i].first.begin(), panes[i].first.end());
        out.push_back(0);
        const u64 record = first_record + 68 * i;
        Put64(out, records + 8 * i, record);
        std::memcpy(out.data() + record + 28, &panes[i].second, 4);
    }
    return out;
}

using Files = std::vector<std::pair<std::string_view, std::vector<u8>>>;

std::vector<u8> Sarc(const Files& files) {
    std::vector<u8> names;
    std::vector<u32> name_offsets;
    for (const auto& [name, data] : files) {
        name_offsets.push_back(static_cast<u32>(names.size()));
        names.insert(names.end(), name.begin(), name.end());
        names.push_back(0);
        while (names.size() % 4 != 0) {
            names.push_back(0);
        }
    }
    const u32 data_offset = static_cast<u32>(0x20 + 16 * files.size() + 8 + names.size());
    std::vector<u8> out(data_offset, 0);
    std::memcpy(out.data(), "SARC", 4);
    std::memcpy(out.data() + 0x0C, &data_offset, 4);
    std::memcpy(out.data() + 0x14, "SFAT", 4);
    const u16 count = static_cast<u16>(files.size());
    std::memcpy(out.data() + 0x1A, &count, 2);
    u32 begin = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const u32 attributes = (1u << 24) | (name_offsets[i] / 4);
        const u32 end = begin + static_cast<u32>(files[i].second.size());
        std::memcpy(out.data() + 0x20 + 16 * i + 4, &attributes, 4);
        std::memcpy(out.data() + 0x20 + 16 * i + 8, &begin, 4);
        std::memcpy(out.data() + 0x20 + 16 * i + 12, &end, 4);
        begin = end;
    }
    const size_t sfnt = 0x20 + 16 * files.size();
    std::memcpy(out.data() + sfnt, "SFNT", 4);
    std::memcpy(out.data() + sfnt + 8, names.data(), names.size());
    for (const auto& [name, data] : files) {
        out.insert(out.end(), data.begin(), data.end());
    }
    return out;
}

// The game's HUD layouts, each twice (the archives repeat layouts), and one other file.
std::vector<u8> HudArchive(f32 left_anchor = -1920.0f) {
    const auto status = Layout({{"N_VRS_00", 1626.0f},
                                {"N_ScaleL_00", left_anchor},
                                {"N_ScaleR_00", 1920.0f}});
    const auto map = Layout({{"N_VRS_00", 1536.0f},
                             {"N_MiniMapCap_00", 1470.0f},
                             {"P_MiniMapFrame_00", 1536.0f},
                             {"S_Floor_00", 1706.0f},
                             {"P_CapDungeon_00", 1536.0f}});
    return Sarc({{"PlayerStatus_00.buil", status},
                 {"MapMenu_00.buil", map},
                 {"PlayerStatus_00.buil", status},
                 {"MapMenu_00.buil", map},
                 {"Other_00.buil", std::vector<u8>(16, 0xAB)}});
}

f32 PaneXAt(const std::vector<u8>& sarc, size_t file, size_t pane) {
    const u32 data_offset = Get32(sarc, 0x0C);
    const u32 begin = Get32(sarc, static_cast<u32>(0x20 + 16 * file + 8));
    f32 x;
    std::memcpy(&x, sarc.data() + data_offset + begin + 0x100 + 68 * pane + 28, 4);
    return x;
}

} // Anonymous namespace

TEST_CASE("The HUD's edge anchors and the minimap move to the 21:9 screen's edges",
          "[oot_ultrawide]") {
    using namespace Core::OotUltrawide;
    STATIC_REQUIRE(EdgeShift == 660.0f);
    auto sarc = HudArchive();
    const auto other = SarcFile(sarc, "Other_00.buil");
    REQUIRE(other.has_value());
    REQUIRE(other->size() == 16);
    REQUIRE(ShiftPanes(sarc, HudEdgeShifts));
    for (const size_t copy : {size_t{0}, size_t{2}}) {
        REQUIRE(PaneXAt(sarc, copy, 0) == 2286.0f);
        REQUIRE(PaneXAt(sarc, copy, 1) == -2580.0f);
        REQUIRE(PaneXAt(sarc, copy, 2) == 2580.0f);
    }
    for (const size_t copy : {size_t{1}, size_t{3}}) {
        REQUIRE(PaneXAt(sarc, copy, 0) == 2196.0f);
        REQUIRE(PaneXAt(sarc, copy, 1) == 2130.0f);
        REQUIRE(PaneXAt(sarc, copy, 2) == 2196.0f);
        REQUIRE(PaneXAt(sarc, copy, 3) == 2366.0f);
        REQUIRE(PaneXAt(sarc, copy, 4) == 2196.0f);
    }
    // Already moved (or another game version): nothing changes.
    const auto moved = sarc;
    REQUIRE_FALSE(ShiftPanes(sarc, HudEdgeShifts));
    REQUIRE(sarc == moved);
}

TEST_CASE("The HUD edge move refuses unexpected layouts", "[oot_ultrawide]") {
    using namespace Core::OotUltrawide;
    auto sarc = HudArchive(-1900.0f);
    const auto original = sarc;
    REQUIRE_FALSE(ShiftPanes(sarc, HudEdgeShifts));
    REQUIRE(sarc == original);
    std::vector<u8> not_sarc(64, 0);
    REQUIRE_FALSE(ShiftPanes(not_sarc, HudEdgeShifts));
    // The archive round trip through a dictionary keeps the edit.
    const std::vector<u8> dictionary(4096, 0x5A);
    const auto compressed =
        Common::Compression::CompressDataZSTDWithDictionary(HudArchive(), dictionary);
    REQUIRE_FALSE(compressed.empty());
    const auto anchored = AnchorHudToEdges(compressed, dictionary);
    REQUIRE(anchored.has_value());
    const auto back =
        Common::Compression::TryDecompressDataZSTDWithDictionary(*anchored, dictionary, 1 << 20);
    REQUIRE(back.has_value());
    REQUIRE(PaneXAt(*back, 3, 1) == 2130.0f);
}
