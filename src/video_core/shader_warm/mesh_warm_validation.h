// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <istream>
#include <span>
#include <stdexcept>

#include "common/common_types.h"

namespace VideoCommon::ShaderWarm {

constexpr u64 MaxMeshWarmRecordBytes = 16 * 1024 * 1024;

inline bool ValidMeshWarmKey(std::span<const u64, 6> hashes, u32 count) {
    return (count == 2 || count == 3) && hashes[0] == 0 && hashes[1] != 0 && hashes[2] == 0 &&
           (hashes[3] != 0) == (count == 3) && hashes[4] == 0 && hashes[5] != 0;
}

/// Preflight the serialized FileEnvironment before it allocates. Leaves the stream at its end.
/// The caller supplies bytes remaining in both the file and its per-record capture budget.
inline u64 InspectMeshWarmEnvironment(std::istream& in, u32 expected_stage, u64 remaining) {
    constexpr u64 HeaderSize = 6 * sizeof(u64) + 7 * sizeof(u32);
    if (remaining < HeaderSize ||
        (expected_stage != 4 && expected_stage != 7 && expected_stage != 8)) {
        throw std::runtime_error("Invalid mesh warm environment header");
    }
    std::array<u64, 6> sizes{};
    std::array<u32, 7> fields{};
    in.read(reinterpret_cast<char*>(sizes.data()), sizeof(sizes));
    in.read(reinterpret_cast<char*>(fields.data()), sizeof(fields));
    const u64 code_size = sizes[0];
    if (!in || fields[6] != expected_stage || code_size < 80 || code_size % 8 != 0 ||
        fields[3] > fields[4] || fields[3] % 8 != 0 || fields[4] % 8 != 0 ||
        u64{fields[4]} - fields[3] >= code_size || fields[2] < fields[3] ||
        u64{fields[2]} + 80 > fields[4]) {
        throw std::runtime_error("Invalid mesh warm environment fields");
    }
    u64 bytes = HeaderSize;
    constexpr std::array<u64, 6> Strides{1, 8, 8, 12, 9, 12};
    for (size_t i = 0; i < sizes.size(); ++i) {
        if (sizes[i] > (remaining - bytes) / Strides[i]) {
            throw std::runtime_error("Oversized mesh warm environment");
        }
        bytes += sizes[i] * Strides[i];
    }
    const u64 trailer = 80 + (expected_stage == 4 ? 0 : 92);
    if (trailer > remaining - bytes) {
        throw std::runtime_error("Truncated mesh warm environment");
    }
    bytes += trailer;
    in.seekg(static_cast<std::streamoff>(bytes - HeaderSize), std::ios::cur);
    if (!in) {
        throw std::runtime_error("Cannot seek mesh warm environment");
    }
    return bytes;
}

} // namespace VideoCommon::ShaderWarm
