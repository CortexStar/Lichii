// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "video_core/hdr_capture.h"

namespace VideoCore::HdrCapture {

namespace {

/// BT.2020 to BT.709 primaries, linear light (ITU-R BT.2087 inverse).
constexpr std::array<f32, 9> BT2020_TO_BT709{
    1.660491f,  -0.587641f, -0.072850f, //
    -0.124550f, 1.132900f,  -0.008349f, //
    -0.018151f, -0.100579f, 1.118730f,
};

f32 HalfToFloat(u16 half) {
    const u32 sign = u32{half & 0x8000u} << 16;
    const u32 exponent = (half >> 10) & 0x1f;
    const u32 mantissa = half & 0x3ffu;
    if (exponent == 0) {
        const f32 value = std::ldexp(static_cast<f32>(mantissa), -24);
        return sign ? -value : value;
    }
    if (exponent == 31) {
        return std::bit_cast<f32>(sign | 0x7f800000u | (mantissa << 13));
    }
    return std::bit_cast<f32>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

/// Tables for the frame conversion (a 2560x1440 frame is 11 million transfer function calls).
constexpr size_t LUT_SIZE = 4096;

struct Tables {
    std::array<f32, LUT_SIZE + 1> pq_nits{};
    std::array<f32, LUT_SIZE + 1> srgb{};
};

f32 Lookup(const std::array<f32, LUT_SIZE + 1>& table, f32 x) {
    const f32 position = std::clamp(x, 0.0f, 1.0f) * static_cast<f32>(LUT_SIZE);
    const size_t index = std::min(static_cast<size_t>(position), LUT_SIZE - 1);
    const f32 fraction = position - static_cast<f32>(index);
    return table[index] + (table[index + 1] - table[index]) * fraction;
}

} // Anonymous namespace

f32 PqToNits(f32 code) {
    constexpr f32 m1 = 2610.0f / 16384.0f;
    constexpr f32 m2 = 2523.0f / 4096.0f * 128.0f;
    constexpr f32 c1 = 3424.0f / 4096.0f;
    constexpr f32 c2 = 2413.0f / 4096.0f * 32.0f;
    constexpr f32 c3 = 2392.0f / 4096.0f * 32.0f;
    const f32 p = std::pow(std::clamp(code, 0.0f, 1.0f), 1.0f / m2);
    return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

f32 LinearToSrgb(f32 linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

f32 Clip(f32 relative) {
    return std::min(relative, 1.0f);
}

namespace {

template <typename Pq, typename Srgb>
void ConvertWith(const f32 rgba[4], const Params& params, u8 bgra[4], Pq&& pq, Srgb&& srgb) {
    std::array<f32, 3> linear{}; // BT.709, relative to reference white
    if (params.color_space == Tegra::ColorSpace::RGB2020_PQ) {
        std::array<f32, 3> bt2020{};
        for (size_t i = 0; i < 3; ++i) {
            bt2020[i] = pq(rgba[i]) / params.reference_white;
        }
        for (size_t i = 0; i < 3; ++i) {
            linear[i] = BT2020_TO_BT709[i * 3] * bt2020[0] +
                        BT2020_TO_BT709[i * 3 + 1] * bt2020[1] +
                        BT2020_TO_BT709[i * 3 + 2] * bt2020[2];
        }
    } else if (params.color_space == Tegra::ColorSpace::RGB709_Linear) {
        for (size_t i = 0; i < 3; ++i) {
            linear[i] = rgba[i] * params.nits_per_unit / params.reference_white;
        }
    } else {
        for (size_t i = 0; i < 3; ++i) {
            bgra[2 - i] = static_cast<u8>(std::lround(std::clamp(rgba[i], 0.0f, 1.0f) * 255.0f));
        }
        bgra[3] = 255;
        return;
    }
    for (f32& value : linear) {
        value = std::max(value, 0.0f); // outside BT.709
    }
    // Above reference white the colour is scaled down by its brightest channel, so a highlight
    // keeps its hue instead of turning yellow or white channel by channel.
    const f32 brightest = std::max({linear[0], linear[1], linear[2]});
    const f32 scale = brightest > 1.0f ? Clip(brightest) / brightest : 1.0f;
    for (size_t i = 0; i < 3; ++i) {
        bgra[2 - i] = static_cast<u8>(std::lround(srgb(linear[i] * scale) * 255.0f));
    }
    bgra[3] = 255;
}

} // Anonymous namespace

void ConvertPixel(const f32 rgba[4], const Params& params, u8 bgra[4]) {
    ConvertWith(rgba, params, bgra, PqToNits, LinearToSrgb);
}

void Convert(std::span<const u16> rgba16f, std::span<u8> bgra8, const Params& params) {
    static const Tables tables = [] {
        Tables result;
        for (size_t i = 0; i <= LUT_SIZE; ++i) {
            const f32 x = static_cast<f32>(i) / static_cast<f32>(LUT_SIZE);
            result.pq_nits[i] = PqToNits(x);
            result.srgb[i] = LinearToSrgb(x);
        }
        return result;
    }();
    const auto pq = [](f32 code) { return Lookup(tables.pq_nits, code); };
    const auto srgb = [](f32 linear) { return Lookup(tables.srgb, linear); };
    const size_t pixels = std::min(rgba16f.size() / 4, bgra8.size() / 4);
    for (size_t i = 0; i < pixels; ++i) {
        const f32 rgba[4]{HalfToFloat(rgba16f[i * 4]), HalfToFloat(rgba16f[i * 4 + 1]),
                          HalfToFloat(rgba16f[i * 4 + 2]), HalfToFloat(rgba16f[i * 4 + 3])};
        ConvertWith(rgba, params, &bgra8[i * 4], pq, srgb);
    }
}

} // namespace VideoCore::HdrCapture
