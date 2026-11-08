// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present_mode_policy.h"

namespace {
using Settings::VSyncMode;
using Vulkan::ChoosePresentPolicy;
using Vulkan::PresentModeSupport;

// AMD's Windows driver: no MAILBOX (vulkaninfo, RX 9070 XT, 26.10.2).
constexpr PresentModeSupport Amd{
    .immediate = true, .mailbox = false, .fifo_relaxed = true, .fifo_latest_ready = true};
constexpr PresentModeSupport Mesa{
    .immediate = true, .mailbox = true, .fifo_relaxed = true, .fifo_latest_ready = false};
constexpr PresentModeSupport FifoOnly{};
} // namespace

TEST_CASE("VRR frame cap sits just below the refresh rate", "[video_core][present_policy]") {
    using Vulkan::VrrFrameCap;
    REQUIRE(VrrFrameCap(240) == 224);
    REQUIRE(VrrFrameCap(144) == 138);
    REQUIRE(VrrFrameCap(120) == 116);
    REQUIRE(VrrFrameCap(60) == 59);
    REQUIRE(VrrFrameCap(360) == 324);
    REQUIRE(VrrFrameCap(0) == 0);
}

TEST_CASE("Unlocked with VRR presents immediately under the cap", "[video_core][present_policy]") {
    const auto policy = ChoosePresentPolicy(VSyncMode::Fifo, true, false, true, 240, Amd);
    REQUIRE(policy.mode == VK_PRESENT_MODE_IMMEDIATE_KHR);
    REQUIRE(policy.frame_cap == 224);
    // FIFO Relaxed asks for VSync as well.
    REQUIRE(ChoosePresentPolicy(VSyncMode::FifoRelaxed, true, false, true, 240, Amd).mode ==
            VK_PRESENT_MODE_IMMEDIATE_KHR);
    // Without a known refresh rate there is no cap, so no immediate presents either.
    const auto unknown = ChoosePresentPolicy(VSyncMode::Fifo, true, false, true, 0, Amd);
    REQUIRE(unknown.mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR);
    REQUIRE(unknown.frame_cap == 0);
}

TEST_CASE("Unlocked without VRR shows the newest frame at each vblank",
          "[video_core][present_policy]") {
    const auto amd = ChoosePresentPolicy(VSyncMode::Fifo, true, false, false, 240, Amd);
    REQUIRE(amd.mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR);
    REQUIRE(amd.frame_cap == 0);
    REQUIRE(ChoosePresentPolicy(VSyncMode::Fifo, true, false, false, 240, Mesa).mode ==
            VK_PRESENT_MODE_MAILBOX_KHR);
    constexpr PresentModeSupport immediate_only{.immediate = true};
    REQUIRE(ChoosePresentPolicy(VSyncMode::Fifo, true, false, false, 240, immediate_only).mode ==
            VK_PRESENT_MODE_IMMEDIATE_KHR);
    REQUIRE(ChoosePresentPolicy(VSyncMode::Fifo, true, false, false, 240, FifoOnly).mode ==
            VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("Explicit VSync modes are kept", "[video_core][present_policy]") {
    for (const bool unlocked : {false, true}) {
        REQUIRE(ChoosePresentPolicy(VSyncMode::Immediate, unlocked, false, false, 240, Amd).mode ==
                VK_PRESENT_MODE_IMMEDIATE_KHR);
        // No MAILBOX on AMD: FIFO, the mode that never tears.
        REQUIRE(ChoosePresentPolicy(VSyncMode::Mailbox, unlocked, false, false, 240, Amd).mode ==
                VK_PRESENT_MODE_FIFO_KHR);
        REQUIRE(ChoosePresentPolicy(VSyncMode::Mailbox, unlocked, false, false, 240, Mesa).mode ==
                VK_PRESENT_MODE_MAILBOX_KHR);
    }
    REQUIRE(ChoosePresentPolicy(VSyncMode::Immediate, false, false, false, 240, FifoOnly).mode ==
            VK_PRESENT_MODE_FIFO_KHR);
    // Speed limit on: the user's VSync choice, still capped inside the VRR range.
    const auto locked = ChoosePresentPolicy(VSyncMode::Fifo, false, false, true, 240, Amd);
    REQUIRE(locked.mode == VK_PRESENT_MODE_FIFO_KHR);
    REQUIRE(locked.frame_cap == 224);
    REQUIRE(ChoosePresentPolicy(VSyncMode::FifoRelaxed, false, false, false, 240, Amd).mode ==
            VK_PRESENT_MODE_FIFO_RELAXED_KHR);
    REQUIRE(ChoosePresentPolicy(VSyncMode::FifoRelaxed, false, false, false, 240, FifoOnly).mode ==
            VK_PRESENT_MODE_FIFO_KHR);
}

TEST_CASE("Frame generation keeps FIFO and no cap", "[video_core][present_policy]") {
    for (const bool vrr : {false, true}) {
        const auto policy = ChoosePresentPolicy(VSyncMode::Fifo, true, true, vrr, 240, Amd);
        REQUIRE(policy.mode == VK_PRESENT_MODE_FIFO_KHR);
        REQUIRE(policy.frame_cap == 0);
        REQUIRE(ChoosePresentPolicy(VSyncMode::FifoRelaxed, true, true, vrr, 240, Amd).mode ==
                VK_PRESENT_MODE_FIFO_RELAXED_KHR);
        REQUIRE(ChoosePresentPolicy(VSyncMode::Immediate, true, true, vrr, 240, Amd).mode ==
                VK_PRESENT_MODE_FIFO_KHR);
    }
}

TEST_CASE("AMD frame generation: VSync off, game held at half the VRR cap",
          "[video_core][present_policy]") {
    using Vulkan::ChooseAmdFrameGenPolicy;
    const auto vrr = ChooseAmdFrameGenPolicy(true, 240, Amd);
    REQUIRE(vrr.mode == VK_PRESENT_MODE_IMMEDIATE_KHR);
    REQUIRE(vrr.frame_cap == 112);
    REQUIRE(ChooseAmdFrameGenPolicy(true, 144, Amd).frame_cap == 69);
    const auto fixed = ChooseAmdFrameGenPolicy(false, 240, Amd);
    REQUIRE(fixed.mode == VK_PRESENT_MODE_IMMEDIATE_KHR);
    REQUIRE(fixed.frame_cap == 0);
    REQUIRE(ChooseAmdFrameGenPolicy(true, 0, Amd).frame_cap == 0);
    REQUIRE(ChooseAmdFrameGenPolicy(true, 240, FifoOnly).mode == VK_PRESENT_MODE_FIFO_KHR);
}
