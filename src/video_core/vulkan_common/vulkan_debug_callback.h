// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/// `validation`: log the validation layer's and driver's messages. `address_bindings`: feed the
/// driver's address binding reports to AddressBindings (EDEN_NX2_GPU_CRUMBS).
vk::DebugUtilsMessenger CreateDebugUtilsCallback(const vk::Instance& instance, bool validation,
                                                 bool address_bindings);

} // namespace Vulkan
