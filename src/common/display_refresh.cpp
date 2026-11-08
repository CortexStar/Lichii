// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/display_refresh.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common {

#ifdef _WIN32
std::optional<u32> QueryDisplayRefreshRate(void* window) {
    if (window == nullptr) {
        return std::nullopt;
    }
    const HMONITOR monitor = MonitorFromWindow(static_cast<HWND>(window), MONITOR_DEFAULTTONULL);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
        return std::nullopt;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    // dmDisplayFrequency is the mode's rate rounded down (239.96 Hz reads 239); 0 and 1 mean the
    // hardware default.
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
        mode.dmDisplayFrequency <= 1) {
        return std::nullopt;
    }
    return static_cast<u32>(mode.dmDisplayFrequency);
}
#else
std::optional<u32> QueryDisplayRefreshRate(void*) {
    return std::nullopt;
}
#endif

} // namespace Common
