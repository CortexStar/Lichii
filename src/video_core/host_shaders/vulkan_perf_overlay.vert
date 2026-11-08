// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 460 core

// The performance overlay's box: one quad, its corners in normalized device coordinates.
layout (push_constant) uniform PushConstants {
    vec4 rect;
};

void main() {
    const vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
}
