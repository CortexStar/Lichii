// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCore::FrameGen {

/// Resource IDs in Lossless.dll for LSFG 3.1's standard and Performance networks.
constexpr u32 LsfgShaderId(u32 performance_id, bool performance) {
    return !performance && performance_id >= 280 && performance_id <= 302 ? performance_id - 23
                                                                          : performance_id;
}

/// Standard quality keeps four feature planes; Performance uses two.
constexpr u32 LsfgFeaturePlanes(bool performance) {
    return performance ? 2U : 4U;
}

} // namespace VideoCore::FrameGen
