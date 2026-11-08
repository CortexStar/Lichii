// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

// The Legend of Zelda: Ocarina of Time at 21:9 (the "Ultrawide 21:9" Add-On). The game lays its
// HUD out on a fixed 1920x1080 canvas and maps it onto the whole frame, which stretches it across
// a 21:9 frame. Instead, the HUD is drawn into the centred 16:9 band, except for panes that span
// the whole canvas (fades, backgrounds, letterbox bars), which keep the full width. Scissors get
// the same transform as viewports. docs: notes/ultrawide-plan.md.
namespace VideoCommon::OotUltrawideHud {

/// The 1920x1080 canvas the game lays the HUD out on.
constexpr f32 CanvasWidth = 1920.0f;
constexpr f32 CanvasHeight = 1080.0f;

/// The centred 16:9 part of a frame: its left edge and width in pixels. A frame that is not wider
/// than 16:9 is its own band.
struct Band {
    u32 x;
    u32 width;

    constexpr bool operator==(const Band&) const = default;
};

[[nodiscard]] constexpr Band HudBand(u32 frame_width, u32 frame_height) {
    const u64 width = (u64{frame_height} * 16 + 4) / 9;
    if (width >= frame_width) {
        return {0, frame_width};
    }
    const u32 band_width = static_cast<u32>(width) & ~1U;
    return {(frame_width - band_width) / 2, band_width};
}

/// A horizontal span in frame pixels (a viewport's x and width, or a scissor's).
struct Span {
    f32 x;
    f32 width;

    constexpr bool operator==(const Span&) const = default;
};

/// Moves a span that covers the frame into the band, keeping its place relative to the frame.
[[nodiscard]] constexpr Span IntoBand(Span span, u32 frame_width, Band band) {
    if (frame_width == 0 || band.width == frame_width) {
        return span;
    }
    const f32 scale = static_cast<f32>(band.width) / static_cast<f32>(frame_width);
    return {static_cast<f32>(band.x) + span.x * scale, span.width * scale};
}

/// The same for an integer scissor: the result covers every pixel the scaled span touches.
struct ScissorSpan {
    s32 x;
    u32 width;

    constexpr bool operator==(const ScissorSpan&) const = default;
};

[[nodiscard]] constexpr ScissorSpan IntoBand(ScissorSpan span, u32 frame_width, Band band) {
    if (frame_width == 0 || band.width == frame_width) {
        return span;
    }
    const s64 left = s64{band.x} + s64{span.x} * band.width / frame_width;
    const s64 right =
        s64{band.x} + ((s64{span.x} + span.width) * band.width + frame_width - 1) / frame_width;
    return {static_cast<s32>(left), static_cast<u32>(std::max<s64>(right - left, 0))};
}

/// A pane that spans the whole canvas width (within half a canvas pixel) keeps the full frame.
[[nodiscard]] constexpr bool IsFullWidthPane(f32 canvas_left, f32 canvas_right) {
    return canvas_left <= 0.5f && canvas_right >= CanvasWidth - 0.5f;
}

/// What the game's world-to-HUD mapping (a 0..1 frame position times 1920, at main+0x20C2B0,
/// main+0x20C99C and main+0x215FB0) has to become so markers stay on their targets inside the
/// band: canvas_x = frame_x * scale - offset.
struct MarkerMap {
    f32 scale;
    f32 offset;

    constexpr bool operator==(const MarkerMap&) const = default;
};

[[nodiscard]] constexpr MarkerMap MarkerMapping(u32 frame_width, Band band) {
    if (band.width == 0) {
        return {CanvasWidth, 0.0f};
    }
    const f32 per_pixel = CanvasWidth / static_cast<f32>(band.width);
    return {static_cast<f32>(frame_width) * per_pixel, static_cast<f32>(band.x) * per_pixel};
}

} // namespace VideoCommon::OotUltrawideHud
