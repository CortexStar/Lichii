// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <optional>

#include "common/common_types.h"

namespace VideoCommon::OotCamera {

/// A camera in world space as AMD FSR frame generation 4 takes it (cameraPosition/Up/Right/
/// Forward, ffx_framegeneration.h PrepareV2), and the vertical field of view the upscalers take.
struct Basis {
    std::array<f32, 3> position{};
    std::array<f32, 3> up{};
    std::array<f32, 3> right{};
    std::array<f32, 3> forward{};
    f32 vertical_fov{}; ///< Radians.
};

namespace Detail {
inline f32 Dot(const std::array<f32, 3>& a, const std::array<f32, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline std::array<f32, 3> Cross(const std::array<f32, 3>& a, const std::array<f32, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline std::array<f32, 3> Scaled(const std::array<f32, 3>& a, f32 s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
inline std::array<f32, 3> Minus(const std::array<f32, 3>& a, const std::array<f32, 3>& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
} // namespace Detail

/// The camera of a row-major view-projection (clip = M * position) whose projection has the
/// usual perspective form: clip w = -view z (so row 3 is minus the view's z row) and no shear
/// between x and y. The game keeps only the product (Ocarina of Time's scene constants, see
/// oot_dlss.h Camera), so the view's rows come back from it: row 3 gives the back axis and its
/// translation; rows 0 and 1, with their share along that axis removed, give right and up and
/// the projection's scales. A y-flipped projection (Vulkan clip space) is turned back into a
/// right-handed basis. Nullopt when the matrix has no such form.
inline std::optional<Basis> FromViewProjection(const std::array<f32, 16>& m) {
    using namespace Detail;
    const std::array<f32, 3> back{-m[12], -m[13], -m[14]};
    const f32 back_length = std::sqrt(Dot(back, back));
    if (!std::isfinite(back_length) || std::abs(back_length - 1.0f) > 1e-2f) {
        return std::nullopt;
    }
    const auto r2 = Scaled(back, 1.0f / back_length);
    const f32 t2 = -m[15] / back_length;
    const std::array<f32, 3> a0{m[0], m[1], m[2]};
    const std::array<f32, 3> a1{m[4], m[5], m[6]};
    const f32 c0 = Dot(a0, r2);
    const f32 c1 = Dot(a1, r2);
    const auto p0 = Minus(a0, Scaled(r2, c0));
    const auto p1 = Minus(a1, Scaled(r2, c1));
    const f32 sx = std::sqrt(Dot(p0, p0));
    const f32 sy = std::sqrt(Dot(p1, p1));
    if (!(sx > 1e-6f) || !(sy > 1e-6f)) {
        return std::nullopt;
    }
    const auto r0 = Scaled(p0, 1.0f / sx);
    auto r1 = Scaled(p1, 1.0f / sy);
    // Rows 0 and 1 hold sx*view_x + c0*view_z (and the same for y), translations included.
    const f32 t0 = (m[3] - c0 * t2) / sx;
    const f32 t1 = (m[7] - c1 * t2) / sy;
    if (std::abs(Dot(r0, r1)) > 1e-2f) {
        return std::nullopt;
    }
    // Camera at -(t0 r0 + t1 r1 + t2 r2): the point the view maps to its origin.
    const std::array<f32, 3> position{-(t0 * r0[0] + t1 * r1[0] + t2 * r2[0]),
                                      -(t0 * r0[1] + t1 * r1[1] + t2 * r2[1]),
                                      -(t0 * r0[2] + t1 * r1[2] + t2 * r2[2])};
    if (Dot(Cross(r0, r1), r2) < 0.0f) {
        r1 = Scaled(r1, -1.0f);
    }
    return Basis{
        .position = position,
        .up = r1,
        .right = r0,
        .forward = Scaled(r2, -1.0f),
        .vertical_fov = 2.0f * std::atan(1.0f / sy),
    };
}

} // namespace VideoCommon::OotCamera
