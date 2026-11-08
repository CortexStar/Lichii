// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace Vulkan::GpuCrumbs {

/// EDEN_NX2_GPU_CRUMBS=1: device-loss diagnostics. The driver reports every GPU address range it
/// binds (VK_EXT_device_address_binding_report), so a fault address names its buffer or image,
/// and AMD buffer markers (VK_AMD_buffer_marker) count each submit, render pass and guest
/// dispatch as it starts and ends, so the log names the last finished and the first unfinished
/// GPU command. =2 also marks every draw. Off by default.
[[nodiscard]] inline int Level() {
    static const int level = [] {
        const char* value = std::getenv("EDEN_NX2_GPU_CRUMBS");
        if (value == nullptr || value[0] < '1' || value[0] > '9') {
            return 0;
        }
        return value[0] - '0';
    }();
    return level;
}

[[nodiscard]] inline bool Requested() {
    return Level() > 0;
}

} // namespace Vulkan::GpuCrumbs
