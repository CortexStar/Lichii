// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "video_core/framebuffer_config.h"

// Screenshots of an HDR frame. The presenter draws the guest's HDR image unchanged (BT.2020 PQ
// code values, or scRGB linear values), which read as 8-bit sRGB look grey and washed out. A
// capture converts it to the SDR range: the luminance the game was told is reference white
// becomes sRGB white (the HUD's white, the top of the game's SDR grade), everything below keeps
// its value, highlights above it are clipped with their hue kept, and BT.2020 colours are mapped
// to BT.709.
namespace VideoCore::HdrCapture {

struct Params {
    Tegra::ColorSpace color_space{Tegra::ColorSpace::RGB2020_PQ};
    f32 reference_white{203.0f}; ///< nits shown as SDR white (what the guest was told)
    f32 nits_per_unit{80.0f};    ///< scRGB: nits per linear 1.0 (FramebufferConfig)
};

/// ST 2084 (PQ) code value in [0, 1] to nits.
f32 PqToNits(f32 code);

/// sRGB transfer function of a linear value in [0, 1].
f32 LinearToSrgb(f32 linear);

/// Luminance relative to reference white, limited to the SDR range.
f32 Clip(f32 relative);

/// One pixel: the presenter's RGBA16F value to 8-bit BGRA.
void ConvertPixel(const f32 rgba[4], const Params& params, u8 bgra[4]);

/// A frame of RGBA16F texels (as RGBA16F half floats, 4 per pixel) to BGRA8.
void Convert(std::span<const u16> rgba16f, std::span<u8> bgra8, const Params& params);

} // namespace VideoCore::HdrCapture
