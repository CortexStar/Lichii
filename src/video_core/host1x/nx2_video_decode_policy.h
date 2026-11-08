// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>

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

/// A host GPU decoder that took this many packets without a picture counts as failed.
constexpr size_t Nx2MaxPacketsWithoutPicture = 32;
/// Packets kept to send again to the next decoder after a GPU decoder fails.
constexpr size_t Nx2MaxReplayPackets = 600;

/// Whether the host GPU decoder failed and the stream must move on to the next decoder: it
/// reported an error, or it never produced a picture.
constexpr bool Nx2GpuDecoderFailed(bool error, bool produced_picture,
                                   size_t packets_without_picture) {
    return error ||
           (!produced_picture && packets_without_picture > Nx2MaxPacketsWithoutPicture);
}

/// Whether an AV1 packet (low-overhead OBU format) carries a sequence header OBU: a decoder
/// can start from it, so the packets before it need not be sent again after a failure.
constexpr bool Nx2Av1StartsSequence(std::span<const u8> data) {
    constexpr u32 SequenceHeader = 1;
    size_t offset = 0;
    while (offset < data.size()) {
        const u8 header = data[offset];
        if (header & 0x80) {
            return false; // forbidden bit: not OBUs
        }
        const u32 type = (header >> 3) & 0xf;
        const bool extension = (header & 0x04) != 0;
        const bool has_size = (header & 0x02) != 0;
        if (type == SequenceHeader) {
            return true;
        }
        offset += extension ? 2 : 1;
        if (!has_size) {
            return false; // the last OBU fills the packet
        }
        u64 size = 0;
        for (u32 i = 0;; ++i) {
            if (offset >= data.size() || i == 8) {
                return false;
            }
            const u8 byte = data[offset++];
            size |= u64{byte & 0x7fU} << (7 * i);
            if (!(byte & 0x80)) {
                break;
            }
        }
        if (size > data.size() - offset) {
            return false;
        }
        offset += static_cast<size_t>(size);
    }
    return false;
}

} // namespace Tegra::Host1x
