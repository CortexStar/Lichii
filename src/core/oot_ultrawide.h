// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string_view>
#include <span>
#include <vector>

#include "common/common_types.h"

// The Legend of Zelda: Ocarina of Time at 21:9: the "Ultrawide 21:9" built-in Add-On
// (file_sys/patch_manager.cpp) makes the game draw a 3440x1440 picture; this is what the rest of
// the system adjusts for it.
namespace Core::OotUltrawide {

constexpr u64 ProgramId = 0x0400486024DEC000ULL;
constexpr std::string_view HeapDefPath = "System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs";

/// The aspect ratio setting asks for 21:9 and the console is docked (handheld keeps the 1080p
/// panel's 16:9).
[[nodiscard]] bool UltrawideRendering();

/// The game's render targets live in the HeapDef's "DynamicTextureAllocatorMem" heap (312320 KiB,
/// inside "Graphics"), sized for its 2560x1440 picture. It grows by its size times the 3440x1440
/// picture's extra pixels, in whole MiB: 312320 KiB * (3440 / 2560 - 1) = 107360 -> 107520 KiB.
constexpr u32 ShippedTextureHeapKiB = 312320;
constexpr u32 TextureHeapExtraKiB = 107520;
/// The application pool and the DRAM grow by as much (in 2 MiB pages), so "Graphics" growing
/// takes nothing from the game's other heaps ("Reserved" is the rest of the pool).
constexpr u64 MemoryExtraBytes = 106ULL << 20;

/// MemoryExtraBytes while a Switch 2 program renders at 21:9, else 0 (KSystemControl).
[[nodiscard]] u64 ApplicationMemoryExtra();

/// The HeapDef table (a zstd-compressed BYML) with "DynamicTextureAllocatorMem" and "Graphics" grown
/// by extra_kib, or nothing when it is not the table this expects.
[[nodiscard]] std::optional<std::vector<u8>> GrowHeapDef(std::span<const u8> compressed,
                                                         u32 extra_kib);

/// The UI layouts (SARC of "BFFH" layout files, zstd against the dictionary "zs.zsdic" in the SARC
/// Pack/ZsDic.pack.zs).
constexpr std::string_view LayoutArchivePath =
    "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs";
constexpr std::string_view DictionaryPackPath = "Pack/ZsDic.pack.zs";
constexpr std::string_view LayoutDictionaryName = "zs.zsdic";

/// A pane of a UI layout moved in x (layout units: the 3840x2160 grid, origin in the middle).
struct PaneShift {
    std::string_view layout; ///< the layout file in the archive, e.g. "PlayerStatus_00.buil"
    std::string_view pane;
    f32 original_x;
    f32 new_x;
};

/// The HUD's own edge anchors: PlayerStatus_00's N_ScaleL_00 (hearts, magic, Stone of Agony) and
/// N_ScaleR_00 (item buttons, shortcut) sit at x = -1920 / +1920, the 16:9 grid's edges. The
/// Add-On's UI canvas squeeze (16/9 over 43/18) puts the grid into the centred 16:9; the 21:9
/// screen's edges are at +-1920 / squeeze = +-2580, so the anchors move out by 660.
constexpr f32 GridHalfWidth = 1920.0f;
constexpr f32 EdgeShift = 660.0f; // 1920 * (43/18) / (16/9) - 1920
inline constexpr PaneShift HudEdgeShifts[]{
    {"PlayerStatus_00.buil", "N_ScaleL_00", -GridHalfWidth, -GridHalfWidth - EdgeShift},
    {"PlayerStatus_00.buil", "N_ScaleR_00", GridHalfWidth, GridHalfWidth + EdgeShift},
};

/// A SARC archive's first file named `name`, or nothing.
[[nodiscard]] std::optional<std::span<const u8>> SarcFile(std::span<const u8> sarc,
                                                          std::string_view name);

/// Moves the panes in every copy of their layout inside the SARC `sarc`, in place. Each pane must be
/// found with its original x (layouts repeat in the archive; every copy is moved). False, with
/// `sarc` unchanged, when one is not.
[[nodiscard]] bool ShiftPanes(std::vector<u8>& sarc, std::span<const PaneShift> shifts);

/// The layout archive (zstd against `dictionary`) with the HUD's edge anchors moved to the 21:9
/// screen's edges (HudEdgeShifts), or nothing when it is not the archive this expects.
[[nodiscard]] std::optional<std::vector<u8>> AnchorHudToEdges(std::span<const u8> compressed,
                                                              std::span<const u8> dictionary);

} // namespace Core::OotUltrawide
