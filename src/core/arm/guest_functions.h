// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include <string_view>

#include "common/common_types.h"

namespace Core {

/// Small hot guest functions the JIT runs as host code (Dynarmic::A64::HostFunction).
enum class GuestFunction : u8 {
    Memmove,           ///< memmove(x0, x1, x2); also memcpy implementations that allow overlap
    Memset,            ///< memset(x0, w1, x2)
    MurmurHash3String, ///< w0 = MurmurHash3_x86_32(x0, strlen(x0), w1)
};

/// A guest function's entry point in one module build, identified by the module's build id (the
/// first 20 bytes, upper-case hex), and its first instructions, checked before it is replaced.
struct GuestFunctionSite {
    std::string_view build_id;
    u64 offset;
    std::array<u32, 4> head;
    GuestFunction function;
};

/// The sites in a module with this build id (empty if none).
std::span<const GuestFunctionSite> GuestFunctionSites(std::string_view build_id);

/// Incremental MurmurHash3_x86_32 (Austin Appleby, public domain), little-endian blocks.
class MurmurHash3X86_32 {
public:
    explicit constexpr MurmurHash3X86_32(u32 seed) : m_hash{seed} {}

    constexpr void Update(std::span<const u8> bytes) {
        for (const u8 byte : bytes) {
            m_tail |= u32{byte} << (8 * (m_length & 3));
            ++m_length;
            if ((m_length & 3) == 0) {
                m_hash ^= Mix(m_tail);
                m_hash = Rotl(m_hash, 13) * 5 + 0xe6546b64;
                m_tail = 0;
            }
        }
    }

    constexpr u32 Finish() const {
        u32 h = m_hash;
        if ((m_length & 3) != 0) {
            h ^= Mix(m_tail);
        }
        h ^= m_length;
        h ^= h >> 16;
        h *= 0x85ebca6b;
        h ^= h >> 13;
        h *= 0xc2b2ae35;
        h ^= h >> 16;
        return h;
    }

private:
    static constexpr u32 Rotl(u32 v, int n) {
        return (v << n) | (v >> (32 - n));
    }
    static constexpr u32 Mix(u32 k) {
        return Rotl(k * 0xcc9e2d51, 15) * 0x1b873593;
    }

    u32 m_hash;
    u32 m_tail{};
    u32 m_length{};
};

} // namespace Core
