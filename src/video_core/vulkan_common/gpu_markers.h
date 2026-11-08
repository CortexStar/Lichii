// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>

namespace Vulkan::GpuMarkers {

/// EDEN_NX2_GPU_MARKERS=1: label every render pass, draw and guest dispatch with
/// VK_EXT_debug_utils (shader hashes as the DKB trace prints them), so that GPU profilers such as
/// Radeon GPU Profiler show the events by name. Off by default; needs no validation layer.
[[nodiscard]] inline bool Requested() {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_GPU_MARKERS");
        return value != nullptr && value[0] == '1';
    }();
    return requested;
}

inline std::atomic_bool enabled{false};

/// Whether labels are recorded: requested and the instance has VK_EXT_debug_utils.
[[nodiscard]] inline bool Enabled() {
    return enabled.load(std::memory_order_relaxed);
}

} // namespace Vulkan::GpuMarkers
