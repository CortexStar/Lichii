// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

namespace VideoCommon {

/// The environment switches of the per-draw debug hooks in RasterizerVulkan::Draw and
/// PrepareDraw. The rasterizer reads them once when it is made (after nx2_switches.txt is in the
/// environment). With none set, a draw skips all its hooks after one member load instead of a
/// static-guard load and a branch for each.
inline constexpr std::array<const char*, 8> DrawDebugSwitches{
    "EDEN_NX2_WINDOW_TRACE", "EDEN_NX2_CB24_TRACE",  "EDEN_NX2_TRACE_VS",
    "EDEN_NX2_SKIP_VS",      "EDEN_NX2_RT_TRACE",    "EDEN_NX2_PENDING_WRITE_TRACE",
    "EDEN_NX2_RENDER_TRACE", "EDEN_DKB_FRAME_TRACE",
};

/// True when `getenv` finds any of DrawDebugSwitches.
template <typename Getenv>
bool AnyDrawDebugSwitch(Getenv&& getenv) {
    for (const char* name : DrawDebugSwitches) {
        if (getenv(name) != nullptr) {
            return true;
        }
    }
    return false;
}

} // namespace VideoCommon
