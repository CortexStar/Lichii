// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 460 core

// The performance overlay: text in an 8x8 bitmap font over a translucent box, and a bar graph of
// the last frame times below it. Sizes match VideoCore::PerfOverlay::Stats and PerfOverlayPass;
// colours arrive encoded for the output (sRGB, PQ or scRGB).

layout (location = 0) out vec4 color;

layout (push_constant) uniform PushConstants {
    vec4 rect;
    vec4 origin_scale; // box top-left in framebuffer pixels, font pixel size
    vec4 text_color;
    vec4 graph_color;
    vec4 hitch_color;
    vec4 background;
};

const int COLUMNS = 34;
const int ROWS = 5;
const int SAMPLES = 136;
const int PAD = 6;
const int ROW_PITCH = 10;
const int TEXT_WIDTH = COLUMNS * 8;
const int GRAPH_TOP = PAD + ROWS * ROW_PITCH + 2;
const int GRAPH_HEIGHT = 40;

layout (std140, binding = 0) uniform OverlayData {
    uvec4 font[64];                         // 128 glyphs of 8 row bytes, 4 bytes per word
    uvec4 text[(ROWS * COLUMNS + 15) / 16]; // 4 characters per word
    vec4 graph[SAMPLES / 4];                // frame time / graph top; negative: a hitch
};

uint ByteAt(uint word, uint index) {
    return (word >> ((index & 3u) * 8u)) & 0xFFu;
}

void main() {
    const ivec2 p = ivec2((gl_FragCoord.xy - origin_scale.xy) / origin_scale.z) - ivec2(PAD);
    vec4 c = background;
    if (p.x >= 0 && p.x < TEXT_WIDTH) {
        if (p.y >= 0 && p.y < ROWS * ROW_PITCH && p.y % ROW_PITCH < 8) {
            const uint index = uint((p.y / ROW_PITCH) * COLUMNS + p.x / 8);
            const uint character = ByteAt(text[index >> 4u][(index >> 2u) & 3u], index);
            const uint byte_index = character * 8u + uint(p.y % ROW_PITCH);
            const uint bits = ByteAt(font[byte_index >> 4u][(byte_index >> 2u) & 3u], byte_index);
            if (((bits >> uint(p.x % 8)) & 1u) != 0u) {
                c = text_color;
            }
        }
        const int graph_y = p.y + PAD - GRAPH_TOP;
        if (graph_y >= 0 && graph_y < GRAPH_HEIGHT) {
            const int sample_index = p.x * SAMPLES / TEXT_WIDTH;
            const float value = graph[sample_index >> 2][sample_index & 3];
            if (float(GRAPH_HEIGHT - graph_y) <= min(abs(value), 1.0) * float(GRAPH_HEIGHT)) {
                c = value < 0.0 ? hitch_color : graph_color;
            } else if (graph_y == GRAPH_HEIGHT / 2) {
                // The average frame time.
                c = mix(background, vec4(text_color.rgb, 1.0), 0.45);
            }
        }
    }
    color = vec4(c.rgb * c.a, c.a);
}
