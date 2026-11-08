// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vulkan/vulkan_core.h>

#include "common/common_types.h"

namespace Vulkan {

/**
 * Whether a depth image is created without TRANSFER_DST, so every write into it goes through a
 * shader. AMD's driver maps GENERAL to every layout usage the image's usage allows, and a copy or
 * resolve destination is outside the layouts its HiZ/HiS metadata is valid in (xgl
 * barrier_policy.cpp InitImageLayoutUsagePolicy, PAL gfx12Image.cpp InitLayoutStateMasks), so with
 * TRANSFER_DST every depth pass ran with HiZ and HiS forced off (PAL gfx12DepthStencilView.cpp,
 * PA_SC_HISZ_RENDER_OVERRIDE). Single-sample depth and depth-stencil attachments only: their
 * uploads, copies, blits, rescales and reinterprets have shader paths, stencil through
 * VK_EXT_shader_stencil_export. `depth_format`: a depth or depth-stencil format (stencil-only
 * images keep their transfers).
 */
[[nodiscard]] constexpr bool DepthWrittenByShader(VkDriverId driver, bool stencil_export,
                                                  bool attachable, u32 num_samples,
                                                  bool depth_format) {
    return driver == VK_DRIVER_ID_AMD_PROPRIETARY && stencil_export && attachable &&
           num_samples == 1 && depth_format;
}

} // namespace Vulkan
