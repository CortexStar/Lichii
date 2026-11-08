// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "video_core/engines/maxwell_3d.h"

namespace VideoCommon {

/// The logic op enable the host records for the guest's registers. `amd_float_workaround`: AMD
/// drivers draw logic ops wrong next to float data (black sewers in Paper Mario: TTYD,
/// 52eab40fc8), so the logic op stays off while any vertex attribute is float. The guest's
/// register keeps its value: the result is host state only.
[[nodiscard]] inline bool HostLogicOpEnable(const Tegra::Engines::Maxwell3D::Regs& regs,
                                            bool amd_float_workaround) {
    if (regs.logic_op.enable == 0) {
        return false;
    }
    if (!amd_float_workaround) {
        return true;
    }
    using AttribType = Tegra::Engines::Maxwell3D::Regs::VertexAttribute::Type;
    return std::none_of(regs.vertex_attrib_format.begin(), regs.vertex_attrib_format.end(),
                        [](const auto& attrib) { return attrib.type == AttribType::Float; });
}

} // namespace VideoCommon
