// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Common {

/// What the host reports for the monitor showing a window.
struct DisplayHdrInfo {
    bool hdr_enabled{};      ///< The desktop runs the monitor in BT.2020 PQ (Windows HDR on).
    float max_luminance{};   ///< Peak luminance in nits from the monitor's EDID (0 if unknown).
    float sdr_white_level{}; ///< Nits the desktop shows SDR white at (0 if unknown).
};

/// Queries the monitor that shows window (an HWND on Windows). Returns nullopt when the platform
/// has no such query or the window is on no known monitor.
std::optional<DisplayHdrInfo> QueryDisplayHdr(void* window);

/// HDR output as reported to a guest that asks for the display's HDR status.
struct HdrOutput {
    bool enabled{};
    u16 peak_luminance{};  ///< nits
    u16 reference_white{}; ///< nits, at most peak_luminance
};

/// Combines the HDR settings with the display state. Off and On decide alone; Auto turns HDR on
/// only while the host reports the monitor in HDR. Luminance settings of 0 follow the monitor.
HdrOutput ResolveHdrOutput(const std::optional<DisplayHdrInfo>& display, Settings::HdrMode mode,
                           u16 peak_setting, u16 reference_white_setting);

} // namespace Common
