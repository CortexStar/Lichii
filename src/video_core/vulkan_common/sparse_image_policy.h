// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vulkan/vulkan_core.h>

namespace Vulkan {

constexpr bool CanUseUnbackedSparseImages(VkDriverId driver, char forced = '\0') {
    if (forced == '0' || forced == '1') {
        return forced == '1';
    }
    return driver != VK_DRIVER_ID_AMD_PROPRIETARY && driver != VK_DRIVER_ID_AMD_OPEN_SOURCE &&
           driver != VK_DRIVER_ID_MESA_RADV;
}

} // namespace Vulkan
