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

bool WindowCoversMonitor(void* window) {
    if (window == nullptr) {
        return false;
    }
    // The render surface may be a child of the top-level window: compare the top level's area.
    const HWND top = GetAncestor(static_cast<HWND>(window), GA_ROOT);
    const HWND target = top != nullptr ? top : static_cast<HWND>(window);
    const HMONITOR monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONULL);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT rect{};
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &info) || !GetWindowRect(target, &rect)) {
        return false;
    }
    const RECT& m = info.rcMonitor;
    return rect.left <= m.left && rect.top <= m.top && rect.right >= m.right &&
           rect.bottom >= m.bottom;
}
#else
std::optional<u32> QueryDisplayRefreshRate(void*) {
    return std::nullopt;
}

bool WindowCoversMonitor(void*) {
    return false;
}
#endif

} // namespace Common
