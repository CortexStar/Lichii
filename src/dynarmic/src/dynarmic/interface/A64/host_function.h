// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace Dynarmic::A64 {

/// The guest state a host function reads and writes.
struct HostFunctionState {
    std::uint64_t* x;        ///< X0-X30
    std::uint64_t* sp;
    std::uint64_t* v;        ///< V0-V31, two words each, low word first
    std::uint32_t fpcr;
    std::uint32_t* fpsr_exc; ///< FPSR cumulative exception bits (FPSR layout): OR new ones in
};

/// Runs a guest function on the host: it reads its arguments and writes its results and every
/// register the guest function would leave changed that its callers can observe. Returns false
/// to decline; the guest's own instructions then run, so a declining function must not have
/// changed anything.
using HostFunction = bool (*)(void* user, const HostFunctionState& state);

struct HostFunctionEntry {
    HostFunction function;
    void* user;
};

}  // namespace Dynarmic::A64
