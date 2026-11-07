// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <bit>

namespace Dynarmic::Backend::X64 {

/// Diagnostic, all hosts. With EDEN_NX2_JIT_MAP=<path> set, every emitted block is appended to
/// <path> as "host_start host_size guest_pc" (hex), and a code cache clear as
/// "clear host_start host_end". Off when the variable is unset.
void JitMapRegister(const void* start, std::size_t size, std::uint64_t guest_pc);
void JitMapClear(const void* begin, const void* end);
void JitMapCounters(const void* jit, const std::uint64_t* counters, std::size_t count);

#if defined(__linux__) && !defined(__ANDROID__)
namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name);
}  // namespace detail
template<typename T>
void PerfMapRegister(T start, const void* end, std::string_view friendly_name) noexcept {
    detail::PerfMapRegister(std::bit_cast<const void*>(start), end, friendly_name);
}
void PerfMapClear();
#else
// Resolve to no-op (compiler thinks fmt has side effects)
template<typename T> inline void PerfMapRegister(T, const void*, std::string_view) noexcept {}
inline void PerfMapClear() noexcept {}
#endif

}  // namespace Dynarmic::Backend::X64
