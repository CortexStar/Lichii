// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>

#include "common/common_types.h"
#include "video_core/engines/maxwell_3d.h"

namespace Vulkan {

inline std::array<f32, 4> GetSurfaceClipViewport(const Tegra::Engines::Maxwell3D::Regs& regs) {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    float y = static_cast<float>(regs.surface_clip.y);
    float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
    if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
        y += height;
        height = -height;
    }
    return {static_cast<float>(regs.surface_clip.x), y,
            (std::max)(1.0f, static_cast<float>(regs.surface_clip.width)), height};
}

} // namespace Vulkan
