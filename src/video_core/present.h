// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"
#include "common/settings.h"

static inline Settings::ScalingFilter GetScalingFilter() {
    return Settings::values.scaling_filter.GetValue();
}

static inline Settings::AntiAliasing GetAntiAliasing() {
    return Settings::values.anti_aliasing.GetValue();
}

static inline Settings::ScalingFilter GetScalingFilterForAppletCapture() {
    return Settings::ScalingFilter::Bilinear;
}

static inline Settings::AntiAliasing GetAntiAliasingForAppletCapture() {
    return Settings::AntiAliasing::None;
}

/// The presented image is an upscaler's anti-aliased output (DLSS, FSR) shown at its own size:
/// a scaling filter (EASU/RCAS, SGSR) has nothing to scale, and the plain sampled copy stays.
constexpr bool SkipsScalingFilter(bool upscaler_output, u32 source_width, u32 source_height,
                                  u32 screen_width, u32 screen_height) {
    return upscaler_output && source_width == screen_width && source_height == screen_height;
}

struct PresentFilters {
    Settings::ScalingFilter (*get_scaling_filter)();
    Settings::AntiAliasing (*get_anti_aliasing)();
};

constexpr PresentFilters PresentFiltersForDisplay{
    .get_scaling_filter = &GetScalingFilter,
    .get_anti_aliasing = &GetAntiAliasing,
};

constexpr PresentFilters PresentFiltersForAppletCapture{
    .get_scaling_filter = &GetScalingFilterForAppletCapture,
    .get_anti_aliasing = &GetAntiAliasingForAppletCapture,
};
