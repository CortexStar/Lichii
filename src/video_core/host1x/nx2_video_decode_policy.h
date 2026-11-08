// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace Tegra::Host1x {

/// Worker threads for a CPU (libdav1d) AV1 decode of a width x height stream at `rate` frames a
/// second (0: not given, taken as 60). libdav1d starts one thread per logical CPU by default and
/// spreads each frame over all of them, so a movie competes with the emulated CPU cores on every
/// core. Two threads per 1080p60 worth of pixels keep up with the stream; never more than half the
/// logical CPUs.
constexpr u32 Nx2SoftwareDecodeThreads(u32 width, u32 height, u32 rate, u32 logical_cpus) {
    constexpr u64 Unit = u64{1920} * 1080 * 60;
    const u64 fps = rate == 0 ? 60 : std::min(rate, 240U);
    const u64 work = u64{std::max(width, 1U)} * std::max(height, 1U) * fps;
    const u64 units = (work + Unit - 1) / Unit;
    const u32 limit = std::max(logical_cpus / 2, 2U);
    return static_cast<u32>(std::clamp<u64>(units * 2, 2, limit));
}

} // namespace Tegra::Host1x
