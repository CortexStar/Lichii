// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Vulkan {

class Device;

/// AMD Anti-Lag 2 (VK_AMD_anti_lag) with its frame limiter: the input stage right before the
/// game's next frame starts (HostVsync; it may block there, for latency and for the frame rate),
/// the present stage with the same frame index right before that frame's present
/// (vkAntiLagUpdateAMD; Cauldron2 device_vk.cpp:1692-1735).
class AntiLag {
public:
    explicit AntiLag(const Device& device);

    [[nodiscard]] bool Available() const noexcept {
        return update != nullptr;
    }

    /// The game is about to start frame `frame` (and sample its input). Blocks for latency and
    /// for `max_fps` (0: no limit).
    void Input(u64 frame, u32 max_fps);
    /// Frame `frame` is about to be presented.
    void Present(u64 frame);

private:
    void Update(u64 frame, u32 max_fps, bool present);

    const Device& device;
    void* update{}; ///< PFN_vkAntiLagUpdateAMD
    std::atomic<u32> last_max_fps{}; ///< the input stage's, repeated at present
};

} // namespace Vulkan
