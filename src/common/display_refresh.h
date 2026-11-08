// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"

namespace Common {

/// Refresh rate in Hz of the monitor that shows window (an HWND on Windows), rounded down.
/// Returns nullopt when the platform has no such query or the window is on no known monitor.
std::optional<u32> QueryDisplayRefreshRate(void* window);

} // namespace Common
