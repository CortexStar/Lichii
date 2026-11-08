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

} // namespace Core
