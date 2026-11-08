// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

// Ocarina of Time's (0400486024DEC000) occlusion query fast path on the split GPU thread's front
// (Maxwell3D::FrontQueryFast): occlusion queries whose results only GPU conditions read
// (render_enable, macro MME DMA reads) get a "visible" result at once instead of a report sent
// to the back, counter resets nothing real reads are dropped, conditions are decided on those
// results, and render_enable writes that change nothing on the back are dropped.
namespace VideoCommon::Nx2QueryFast {

/// 3D methods the front's query fast path looks at: MME_DMA_READ(_FIFOED) (0x157/0x158),
/// clear_report_value (0x54c), render_enable.mode (0x556) and report_semaphore.query (0x6c3).
[[nodiscard]] constexpr bool Watches(u32 method) {
    return method == 0x6c3 || method == 0x556 || method == 0x54c || method == 0x157 ||
           method == 0x158;
}

} // namespace VideoCommon::Nx2QueryFast
