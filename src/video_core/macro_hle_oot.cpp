// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/gpu_split.h"
#include "video_core/macro_hle_oot.h"

namespace Tegra::OotMacros {

namespace {

constexpr u32 MaxInstructions = 1024;

// TODO(OoT macros): no replacement is registered yet. Ocarina's macro code is uploaded by the
// game's NVN2 driver at run time and no dump of it existed when this was written, so none of
// the hot macros (methods 0xe02, 0xe1c, 0xe3a, 0xe36, 0xe2c) could be reproduced exactly.
// To add one:
//   1. Run Ocarina with EDEN_NX2_OOT_MACROS_DUMP=1 (split GPU thread with front macros, the
//      default) and filter the log for "OoT macro". Each macro's code is logged once per hash
//      ("OoT macro code"), and the first calls of the hot macros with their parameters and
//      the writes they emitted ("OoT macro call").
//   2. Decode the code with TuringMacroInterpreter::Decode (macro_turing.h; Mesa's
//      mme_tu104 disassembler reads the same format) and write the handler from it: the same
//      writes in the same order, FIFO consumption, data RAM effects and DMA reads (0x157/0x158
//      through Context::Emit). Registers it reads come from Context::Read.
//   3. Check it against the logged calls, then add {hash, name, handler} below.
constexpr std::array<Macro, 0> macros{};

[[nodiscard]] bool IsHot(u32 method) {
    switch (method) {
    case 0xe02:
    case 0xe1c:
    case 0xe2c:
    case 0xe36:
    case 0xe3a:
        return true;
    default:
        return false;
    }
}

} // Anonymous namespace

bool HasHandlers() {
    return !macros.empty();
}

u64 HashMacro(std::span<const u32> code, u32 start) {
    // FNV-1a over the words, plus the instruction count.
    u64 hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](u32 word) {
        for (u32 byte = 0; byte < 4; ++byte) {
            hash ^= (word >> (byte * 8)) & 0xff;
            hash *= 0x100000001b3ULL;
        }
    };
    u32 count = 0;
    bool end_next = false;
    for (u32 pc = start; count < MaxInstructions && size_t{pc} * 3 + 2 < code.size(); ++pc) {
        mix(code[pc * 3]);
        mix(code[pc * 3 + 1]);
        mix(code[pc * 3 + 2]);
        ++count;
        if (end_next) {
            break;
        }
        // Bit 0 of the instruction's high word: end after the next instruction (delay slot).
        end_next = (code[pc * 3 + 2] & 1) != 0;
    }
    mix(count);
    return hash;
}

const Macro* Find(u64 hash) {
    for (const Macro& macro : macros) {
        if (macro.hash == hash) {
            return &macro;
        }
    }
    return nullptr;
}

bool DumpEnabled() {
    static const bool enabled = VideoCommon::Nx2Switch("EDEN_NX2_OOT_MACROS_DUMP", false);
    return enabled;
}

void DumpCode(u32 method, u32 start, std::span<const u32> code) {
    static std::unordered_set<u64> dumped;
    const u64 hash = HashMacro(code, start);
    if (!dumped.insert(hash).second) {
        return;
    }
    u32 count = 0;
    bool end_next = false;
    for (u32 pc = start; count < MaxInstructions && size_t{pc} * 3 + 2 < code.size(); ++pc) {
        ++count;
        if (end_next) {
            break;
        }
        end_next = (code[pc * 3 + 2] & 1) != 0;
    }
    LOG_INFO(HW_GPU, "OoT macro code: method {:#x} start {:#x} hash {:016x} instructions {}",
             method, start, hash, count);
    // Eight instructions (24 words, in code order) a line.
    for (u32 first = 0; first < count; first += 8) {
        std::string line;
        for (u32 i = first; i < std::min(count, first + 8); ++i) {
            const u32 pc = start + i;
            line += fmt::format(" {:08x}{:08x}{:08x}", code[pc * 3 + 2], code[pc * 3 + 1],
                                code[pc * 3]);
        }
        LOG_INFO(HW_GPU, "OoT macro code: {:016x} +{:#x}:{}", hash, first, line);
    }
}

bool WantCallLog(u32 method) {
    if (!IsHot(method)) {
        return false;
    }
    static std::unordered_map<u32, u32> logged;
    return logged[method]++ < 8;
}

void LogCall(u32 method, u32 start, std::span<const u32> parameters, std::span<const u32> writes) {
    std::string params;
    for (const u32 parameter : parameters) {
        params += fmt::format(" {:x}", parameter);
    }
    std::string emitted;
    for (size_t i = 0; i + 1 < writes.size(); i += 2) {
        emitted += fmt::format(" {:x}={:x}", writes[i], writes[i + 1]);
    }
    LOG_INFO(HW_GPU, "OoT macro call: method {:#x} start {:#x} parameters{} writes{}", method,
             start, params, emitted);
}

} // namespace Tegra::OotMacros
