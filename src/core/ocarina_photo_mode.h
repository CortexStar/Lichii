// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>

#include "common/common_types.h"

namespace Core {
class System;
}

// The Legend of Zelda: Ocarina of Time's photo mode: a free camera, a frozen world, no HUD, picture
// ratios, hotsampling and the IGCS camera tools interface. Offsets belong to the game's main with
// build id E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348 (docs/nx2/oot-photo-mode.md).
namespace Core::OcarinaPhotoMode {

constexpr u64 ProgramId = 0x0400486024DEC000ULL;

/// The game's time manager: **(main + TimeManagerSlot). Each frame main+0x3b4e300 turns the frame
/// step into the deltas every system reads, multiplied by the global scale at +0x1c0 (and by one of
/// 16 per-channel scales at +0x140).
constexpr u64 TimeManagerSlot = 0x4c7e890;
constexpr u64 GlobalTimeScaleOffset = 0x1c0;
/// Near zero rather than zero: the game also takes the reciprocal of the scaled step.
constexpr u32 FrozenTimeScale = 0x38d1b717; // 1e-4f

/// The camera override: the function around main+0x2bce000 takes the active camera object (x21,
/// from the camera manager at main+0x2bcedac) and builds the render view and the camera copies from
/// it: the world-to-view matrix at +0x8 (3 rows of 4 floats), eye, target and up at +0x38, the
/// projection at +0x9c (and a copy at +0xdc), and the vertical field of view with its sin, cos and
/// tan at +0x130. The built-in "Photo Mode" patch branches from main+0x2bcedc0, right after the
/// object is fetched, to a cave that stores the object's address and, while enabled, writes the
/// state's camera over the object's before anything reads it.
constexpr u64 SiteOffset = 0x2bcedc0;
constexpr u64 CaveOffset = 0x3C46420;
constexpr u64 StateOffset = 0x4E9FE40; ///< BSS words no code references
constexpr u64 StateSize = 0x90;
constexpr u64 EnabledOffset = 0x0;        ///< u32, nonzero: use the camera below
constexpr u64 EyeOffset = 0x8;            ///< eye, target, up: 9 floats
constexpr u64 CameraPointerOffset = 0x30; ///< u64, written by the cave: the active camera object
constexpr u64 ViewMatrixOffset = 0x38;    ///< 3 rows of 4 floats (world to view, LookAt)
constexpr u64 FovOffset = 0x70;           ///< vertical field of view (radians), its sin, cos, tan
constexpr u64 ProjectionXOffset = 0x80;   ///< f32, the projection's [0][0] for that field of view
constexpr u64 ProjectionYOffset = 0x84;   ///< f32, the projection's [1][1]
constexpr u64 FovEnabledOffset = 0x88;    ///< u32, nonzero: also write the field of view
constexpr u64 CameraEyeOffset = 0x38;     ///< in the camera object: eye, target, up
constexpr u64 CameraFovOffset = 0x130;    ///< in the camera object: field of view, sin, cos, tan
constexpr u64 CameraAspectOffset = 0x140; ///< in the camera object

/// b CaveOffset (at SiteOffset).
constexpr std::array<u8, 4> SiteBranch{0x98, 0xDD, 0x41, 0x14};
/// ldr x8, [x24, #0x270]
constexpr std::array<u8, 4> SiteOriginal{0x08, 0x3B, 0x41, 0xF9};
/// adrp x9, state page; add x9, x9, #0xe40; str x21, [x9, #0x30]; ldr w10, [x9]; cbz w10, out;
/// the matrix (state+0x38, 48 bytes) to camera+0x8; eye, target, up (state+0x8, 36 bytes) to
/// camera+0x38; if state+0x88: field of view (state+0x70, 16 bytes) to camera+0x130 and the
/// projections' [0][0] and [1][1] (state+0x80, +0x84) to camera+0x9c/+0xdc and +0xb0/+0xf0;
/// out: ldr x8, [x24, #0x270]; b main+0x2bcedc4
constexpr std::array<u8, 116> Cave{
    0xC9, 0x92, 0x00, 0xB0, 0x29, 0x01, 0x39, 0x91, 0x35, 0x19, 0x00, 0xF9, 0x2A, 0x01, 0x40, 0xB9,
    0xEA, 0x02, 0x00, 0x34, 0x20, 0x81, 0xC3, 0x3C, 0xA0, 0x82, 0x80, 0x3C, 0x20, 0x81, 0xC4, 0x3C,
    0xA0, 0x82, 0x81, 0x3C, 0x20, 0x81, 0xC5, 0x3C, 0xA0, 0x82, 0x82, 0x3C, 0x2A, 0xAD, 0x40, 0xA9,
    0xAA, 0xAE, 0x03, 0xA9, 0x2A, 0xAD, 0x41, 0xA9, 0xAA, 0xAE, 0x04, 0xA9, 0x2A, 0x29, 0x40, 0xB9,
    0xAA, 0x5A, 0x00, 0xB9, 0x2A, 0x89, 0x40, 0xB9, 0x2A, 0x01, 0x00, 0x34, 0x20, 0x1D, 0xC0, 0x3D,
    0xA0, 0x4E, 0x80, 0x3D, 0x2A, 0x81, 0x40, 0xB9, 0xAA, 0x9E, 0x00, 0xB9, 0xAA, 0xDE, 0x00, 0xB9,
    0x2A, 0x85, 0x40, 0xB9, 0xAA, 0xB2, 0x00, 0xB9, 0xAA, 0xF2, 0x00, 0xB9, 0x08, 0x3B, 0x41, 0xF9,
    0x4D, 0x22, 0xBE, 0x17};

[[nodiscard]] constexpr u32 Word(const std::array<u8, 4>& bytes) {
    return u32{bytes[0]} | (u32{bytes[1]} << 8) | (u32{bytes[2]} << 16) | (u32{bytes[3]} << 24);
}

/// Picture ratios photo mode crops to (L3 cycles them); {0, 0} is the game's own frame.
constexpr std::array<std::array<u32, 2>, 9> PictureRatios{{
    {0, 0}, {21, 9}, {32, 9}, {3, 2}, {4, 3}, {1, 1}, {4, 5}, {2, 3}, {9, 16}}};

/// The centred crop of a width x height frame to ratio: x, y, width, height (even sizes).
[[nodiscard]] inline std::array<u32, 4> CropToRatio(u32 width, u32 height,
                                                    const std::array<u32, 2>& ratio) {
    if (ratio[0] == 0 || ratio[1] == 0 || width == 0 || height == 0) {
        return {0, 0, width, height};
    }
    u64 crop_width = width;
    u64 crop_height = height;
    if (u64{width} * ratio[1] > u64{height} * ratio[0]) {
        crop_width = u64{height} * ratio[0] / ratio[1];
    } else {
        crop_height = u64{width} * ratio[1] / ratio[0];
    }
    crop_width = (crop_width & ~u64{1}) > 0 ? (crop_width & ~u64{1}) : 1;
    crop_height = (crop_height & ~u64{1}) > 0 ? (crop_height & ~u64{1}) : 1;
    return {static_cast<u32>((width - crop_width) / 2), static_cast<u32>((height - crop_height) / 2),
            static_cast<u32>(crop_width), static_cast<u32>(crop_height)};
}

/// Asks the next present to switch photo mode on or off (the hotkey; L3 + R3 on the controller).
void RequestToggle();
/// Whether photo mode is on.
[[nodiscard]] bool Active();
/// The next picture ratio (the hotkey; L3 on the controller).
void RequestRatioCycle();
/// The ratio photo mode crops pictures to now, {0, 0} for the whole frame (or photo mode off).
[[nodiscard]] std::array<u32, 2> PictureRatio();
/// Hotsampling (Minus in photo mode, or the hotkey): the frontend sizes its window to the picture
/// (the ratio's crop at the internal resolution) and the presenter shows only that crop, pixel for
/// pixel, so tools that capture the window (ReShade) get the whole picture.
void RequestHotsampleToggle();
[[nodiscard]] bool HotsampleActive();
/// Whether the controller asked for a picture since the last call (Plus in photo mode).
[[nodiscard]] bool TakeCaptureRequest();
/// Once per present: switches photo mode on or off on request, and while it is on reads the
/// controller and moves the camera.
void Update(Core::System& system);
/// Leaves photo mode without touching the game's memory (a game is starting).
void Reset();

/// IGCS camera tools for the IGCS Connector add-on for ReShade (ocarina_igcs.cpp exports the
/// functions it looks for). Photo mode applies the moves at its next update.
namespace Igcs {
struct CameraData {
    bool enabled{};
    bool session{};
    f32 fov_degrees{};
    std::array<f32, 3> eye{};
    std::array<f32, 4> quaternion{};
    std::array<f32, 3> up{};
    std::array<f32, 3> right{};
    std::array<f32, 3> forward{};
    f32 pitch{};
    f32 yaw{};
    f32 roll{};
};
/// 0 started, 1 photo mode is off, 3 a session is already active.
int StartSession(u8 type);
void MovePanorama(f32 step_angle);
void MoveMultishot(f32 step_left_right, f32 step_up_down, f32 fov_degrees, bool from_start);
void EndSession();
/// Calls the add-on's connectFromCameraTools() if ReShade loaded it.
bool Connect();
void WriteCameraData(const CameraData& data);
} // namespace Igcs

/// World to view rows (right, up, back; translation -R * eye), the game's LookAt convention.
inline std::array<f32, 12> LookAt(const std::array<f32, 3>& eye, const std::array<f32, 3>& target,
                                  const std::array<f32, 3>& world_up) {
    const auto sub = [](const std::array<f32, 3>& a, const std::array<f32, 3>& b) {
        return std::array<f32, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto cross = [](const std::array<f32, 3>& a, const std::array<f32, 3>& b) {
        return std::array<f32, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                                  a[0] * b[1] - a[1] * b[0]};
    };
    const auto dot = [](const std::array<f32, 3>& a, const std::array<f32, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto normalize = [&](std::array<f32, 3> v) {
        const f32 length = std::sqrt(dot(v, v));
        if (length > 1e-6f) {
            v = {v[0] / length, v[1] / length, v[2] / length};
        }
        return v;
    };
    const auto back = normalize(sub(eye, target));
    const auto right = normalize(cross(world_up, back));
    const auto up = cross(back, right);
    return {right[0], right[1], right[2], -dot(right, eye), up[0], up[1], up[2], -dot(up, eye),
            back[0],  back[1],  back[2],  -dot(back, eye)};
}

} // namespace Core::OcarinaPhotoMode
