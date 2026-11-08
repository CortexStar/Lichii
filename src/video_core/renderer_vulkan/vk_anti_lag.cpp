// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_anti_lag.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

AntiLag::AntiLag(const Device& device_) : device{device_} {
    if (Settings::values.anti_lag.GetValue()) {
        update = reinterpret_cast<void*>(device.GetAntiLagUpdate());
    }
}

void AntiLag::Update(u64 frame, u32 max_fps, bool present) {
    const auto function = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(update);
    const VkAntiLagPresentationInfoAMD presentation{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,
        .pNext = nullptr,
        .stage = present ? VK_ANTI_LAG_STAGE_PRESENT_AMD : VK_ANTI_LAG_STAGE_INPUT_AMD,
        .frameIndex = frame,
    };
    const VkAntiLagDataAMD data{
        .sType = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,
        .pNext = nullptr,
        .mode = VK_ANTI_LAG_MODE_ON_AMD,
        .maxFPS = max_fps,
        .pPresentationInfo = &presentation,
    };
    function(*device.GetLogical(), &data);
}

void AntiLag::Input(u64 frame, u32 max_fps) {
    if (update != nullptr) {
        last_max_fps.store(max_fps, std::memory_order_relaxed);
        Update(frame, max_fps, false);
    }
}

void AntiLag::Present(u64 frame) {
    if (update != nullptr && frame != 0) {
        Update(frame, last_max_fps.load(std::memory_order_relaxed), true);
    }
}

} // namespace Vulkan
