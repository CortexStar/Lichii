// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cmath>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "video_core/oot_camera.h"

namespace {

using Matrix = std::array<float, 16>;
using Vector = std::array<float, 3>;

Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            for (int k = 0; k < 4; ++k) {
                out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
            }
        }
    }
    return out;
}

/// A right-handed view (rows right, up, back) of a camera at `eye` turned by `yaw` about y and
/// pitched by `pitch`, and a GL perspective projection.
Matrix ViewProjection(Vector eye, float yaw, float pitch, float fov, bool flip_y) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const Vector right{cy, 0.0f, -sy};
    const Vector up{sy * sp, cp, cy * sp};
    const Vector back{sy * cp, -sp, cy * cp};
    const auto dot = [](Vector a, Vector b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const Matrix view{right[0], right[1], right[2], -dot(right, eye),
                      up[0],    up[1],    up[2],    -dot(up, eye),
                      back[0],  back[1],  back[2],  -dot(back, eye),
                      0.0f,     0.0f,     0.0f,     1.0f};
    const float f = 1.0f / std::tan(fov / 2.0f);
    const float near_plane = 0.1f, far_plane = 25000.0f, aspect = 16.0f / 9.0f;
    const Matrix projection{f / aspect, 0.0f, 0.0f, 0.0f,
                            0.0f, flip_y ? -f : f, 0.0f, 0.0f,
                            0.0f, 0.0f, (far_plane + near_plane) / (near_plane - far_plane),
                            2.0f * far_plane * near_plane / (near_plane - far_plane),
                            0.0f, 0.0f, -1.0f, 0.0f};
    return Multiply(projection, view);
}

void CheckVector(const Vector& actual, const Vector& expected) {
    for (int i = 0; i < 3; ++i) {
        CHECK(actual[i] == Catch::Approx(expected[i]).margin(1e-3));
    }
}

} // Anonymous namespace

TEST_CASE("OoT camera basis from the view-projection", "[video_core][oot]") {
    using VideoCommon::OotCamera::FromViewProjection;
    const float fov = 50.0f * std::numbers::pi_v<float> / 180.0f;
    const Vector eye{120.0f, 35.0f, -480.0f};
    const float yaw = 0.7f, pitch = -0.2f;
    for (const bool flip_y : {false, true}) {
        const auto basis = FromViewProjection(ViewProjection(eye, yaw, pitch, fov, flip_y));
        REQUIRE(basis);
        CheckVector(basis->position, eye);
        CheckVector(basis->right, {std::cos(yaw), 0.0f, -std::sin(yaw)});
        CheckVector(basis->up, {std::sin(yaw) * std::sin(pitch), std::cos(pitch),
                                std::cos(yaw) * std::sin(pitch)});
        CheckVector(basis->forward, {-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                                     -std::cos(yaw) * std::cos(pitch)});
        CHECK(basis->vertical_fov == Catch::Approx(fov).margin(1e-4));
    }
}

TEST_CASE("OoT camera basis rejects non-perspective matrices", "[video_core][oot]") {
    using VideoCommon::OotCamera::FromViewProjection;
    std::array<float, 16> identity{};
    identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
    CHECK(!FromViewProjection(identity));
    CHECK(!FromViewProjection(std::array<float, 16>{}));
}
