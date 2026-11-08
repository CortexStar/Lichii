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

} // namespace Core::OotUltrawide
