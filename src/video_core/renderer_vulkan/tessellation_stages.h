// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Vulkan {

/// Whether a graphics pipeline has tessellation stages, from what sits in the tessellation
/// control and evaluation slots. A task/mesh pipeline keeps its mesh program in a tessellation
/// slot when it has a task program, so a mesh pipeline never has tessellation.
constexpr bool HasTessellationStages(bool mesh_pipeline, bool tess_control_slot,
                                     bool tess_eval_slot) {
    return !mesh_pipeline && (tess_control_slot || tess_eval_slot);
}

} // namespace Vulkan
