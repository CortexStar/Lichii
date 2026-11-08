// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Vulkan {

/// Present modes the surface lists (FIFO is always there).
struct PresentModeSupport {
    bool immediate{};
    bool mailbox{};
    bool fifo_relaxed{};
    bool fifo_latest_ready{}; ///< Listed and VK_KHR_present_mode_fifo_latest_ready enabled.
};

struct PresentPolicy {
    VkPresentModeKHR mode{VK_PRESENT_MODE_FIFO_KHR};
    u32 frame_cap{}; ///< Presents per second to hold the game below, 0 for none.
};

/// Frame cap inside a VRR display's range: floor(R - R^2 / 3600), 224 at 240 Hz, 138 at 144 Hz.
/// At or above the maximum refresh VRR has nothing left to adjust. The margin grows with the rate
/// because frame-time jitter takes a larger share of a shorter refresh period.
constexpr u32 VrrFrameCap(u32 refresh_hz) {
    // R - ceil(R^2 / 3600) is the floor of R - R^2 / 3600 in integers.
    return refresh_hz == 0
               ? 0
               : static_cast<u32>(refresh_hz - (u64{refresh_hz} * refresh_hz + 3599) / 3600);
}

/// Picks the swapchain's present mode and the frame cap for the path without AMD's frame
/// generation swapchain.
/// @param requested  The user's VSync mode.
/// @param unlocked   The speed limit is off (or turbo): the game may run faster than the display.
/// @param frame_gen  Lichii's own frame generation presents extra frames.
/// @param vrr        The display runs with variable refresh.
/// @param refresh_hz Refresh rate of the display, 0 when unknown.
/// @param support    Present modes the surface offers.
constexpr PresentPolicy ChoosePresentPolicy(Settings::VSyncMode requested, bool unlocked,
                                            bool frame_gen, bool vrr, u32 refresh_hz,
                                            PresentModeSupport support) {
    using Settings::VSyncMode;
    const u32 cap = vrr ? VrrFrameCap(refresh_hz) : 0;
    if (frame_gen) {
        // Generated frames are spaced by the display's vblank, and a cap on presents would count
        // them as game frames.
        return {requested == VSyncMode::FifoRelaxed && support.fifo_relaxed
                    ? VK_PRESENT_MODE_FIFO_RELAXED_KHR
                    : VK_PRESENT_MODE_FIFO_KHR,
                0};
    }
    if (unlocked && (requested == VSyncMode::Fifo || requested == VSyncMode::FifoRelaxed)) {
        // With VRR, the cap keeps every present inside the display's range, so an immediate
        // present scans out at once without tearing. Without VRR, the newest frame at each vblank:
        // FIFO_LATEST_READY drops older frames like MAILBOX and is what AMD's Windows driver has.
        if (vrr && cap != 0 && support.immediate) {
            return {VK_PRESENT_MODE_IMMEDIATE_KHR, cap};
        }
        if (support.fifo_latest_ready) {
            return {VK_PRESENT_MODE_FIFO_LATEST_READY_KHR, cap};
        }
        if (support.mailbox) {
            return {VK_PRESENT_MODE_MAILBOX_KHR, cap};
        }
        if (support.immediate) {
            return {VK_PRESENT_MODE_IMMEDIATE_KHR, cap};
        }
        return {VK_PRESENT_MODE_FIFO_KHR, cap};
    }
    switch (requested) {
    case VSyncMode::Immediate:
        if (support.immediate) {
            return {VK_PRESENT_MODE_IMMEDIATE_KHR, cap};
        }
        return {support.mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR, cap};
    case VSyncMode::Mailbox:
        return {support.mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR, cap};
    case VSyncMode::FifoRelaxed:
        return {support.fifo_relaxed ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR,
                cap};
    case VSyncMode::Fifo:
    default:
        return {VK_PRESENT_MODE_FIFO_KHR, cap};
    }
}

} // namespace Vulkan
