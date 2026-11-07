// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"

namespace Tegra::OotMacros {

// Native replacements for Ocarina of Time's hottest 3D macros (NVN2 Turing MME code, run on the
// split GPU thread's front by Maxwell3D::FrontRunMacro). A replacement is found by the hash of
// the macro's code (HashMacro), so it only runs for the exact code it was written against.
// EDEN_NX2_OOT_MACROS=0 runs every macro through the JIT again.
//
// A handler must make exactly the writes the macro makes, in the same order, with the same
// values (Emit), consume the load FIFO the same way, and leave the data RAM as the macro would.

/// What a handler runs against (the same view the JIT gets in FrontRunMacro).
struct Context {
    /// The load FIFO: the call's parameters; MME_DMA_READ_FIFOED (method 0x158) appends to it.
    std::vector<u32>& parameters;
    /// The channel's MME data RAM (shared with its compute engine).
    std::span<u32> data_ram;
    void* user;
    u32 (*read)(void*, u32);        ///< (user, method)
    void (*emit)(void*, u32, u32);  ///< (user, method, value)

    /// The 3D register as the macro would read it (the front's copy).
    [[nodiscard]] u32 Read(u32 method) const {
        return read(user, method);
    }
    /// A method write of the macro (methods >= 0xe00 are not allowed). 0x157/0x158 run the DMA
    /// read like the JIT's emit does.
    void Emit(u32 method, u32 value) const {
        emit(user, method, value);
    }
};

using Handler = void (*)(Context& context);

struct Macro {
    u64 hash;
    const char* name;
    Handler handler;
};

/// Whether any replacement is registered (none: FrontRunMacro skips the lookup).
[[nodiscard]] bool HasHandlers();

/// Hash of a macro's code: its instructions (three words each) from `start` through the first
/// one with the end bit and its delay slot, at most 1024 instructions. Stable across builds.
[[nodiscard]] u64 HashMacro(std::span<const u32> code, u32 start);

/// The replacement for the macro with code hash `hash`, or nullptr.
[[nodiscard]] const Macro* Find(u64 hash);

// EDEN_NX2_OOT_MACROS_DUMP=1 (diagnostic): logs each macro's code once per hash, and the
// parameters and emitted writes of the first calls of the hot macros, for writing replacements.
[[nodiscard]] bool DumpEnabled();
/// Logs `method`'s code (once per hash).
void DumpCode(u32 method, u32 start, std::span<const u32> code);
/// Whether this call of `method` should be logged with LogCall.
[[nodiscard]] bool WantCallLog(u32 method);
/// `writes`: method, value pairs in emission order.
void LogCall(u32 method, u32 start, std::span<const u32> parameters, std::span<const u32> writes);

} // namespace Tegra::OotMacros
