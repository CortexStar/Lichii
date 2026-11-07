// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

// What Ocarina of Time's photo mode (core/ocarina_photo_mode) asks of the renderer and the
// presenter. All of it is off unless photo mode is on.
namespace VideoCore::PhotoMode {

/// The draws into the game's anti-aliased image after its anti-aliasing that have no depth and
/// blend (the HUD) are skipped while set.
void SetHideHud(bool hide);
[[nodiscard]] bool HideHud();

/// The GPU address of the image the game's anti-aliasing output pass writes (the HUD's target).
void SetOutputImage(u64 gpu_address);
[[nodiscard]] u64 OutputImage();

/// The compute pass that lays the scene's haze over it is skipped while set.
void SetHideHaze(bool hide);
[[nodiscard]] bool HideHaze();

/// Hotsampling: the presenter shows only the centred crop of the game's frame at this ratio
/// (width:height; 0:0 is off), and the frontend sizes its window to it.
void SetPresentCrop(u32 ratio_width, u32 ratio_height);
/// Width in the low 32 bits, height in the high; 0 when off.
[[nodiscard]] u64 PresentCrop();

/// The size of the largest layer the game presented last, after the crop (screenshots at the
/// game's own size). Width in the low 32 bits, height in the high; 0 before the first frame.
void SetPresentedFrameSize(u32 width, u32 height);
[[nodiscard]] u64 PresentedFrameSize();

} // namespace VideoCore::PhotoMode
