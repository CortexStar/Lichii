// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "core/arm/guest_functions.h"

namespace Core {

namespace {

// The "sdk" module (libc, nn::os) of Ocarina of Time version 131072.
constexpr std::string_view Sdk1643 = "1643A838814F9C10C696F930ED681E7FA3FB9AF8";

constexpr std::array Sites{
    // memcpy: a branch to __memcpy_aarch64 through the PLT.
    GuestFunctionSite{Sdk1643, 0x78F020, {0x1403B778, 0x00000000, 0x00000000, 0x00000000},
                      GuestFunction::Memmove},
    GuestFunctionSite{Sdk1643, 0x78F040, {0xEB01001F, 0x54000BA0, 0x8B020028, 0xEB00011F},
                      GuestFunction::Memmove},
    GuestFunctionSite{Sdk1643, 0x78F300, {0xB40006C2, 0x8B020008, 0x39000001, 0x381FF101},
                      GuestFunction::Memset},
    // __memcpy_aarch64 and __memcpy_aarch64_simd (Arm optimized-routines): both copy
    // overlapping buffers as memmove does (backwards when dst - src < n).
    GuestFunctionSite{Sdk1643, 0x7BCDC0, {0x8B020024, 0x8B020005, 0xF102005F, 0x540007A8},
                      GuestFunction::Memmove},
    GuestFunctionSite{Sdk1643, 0x7BD000, {0x8B020024, 0x8B020005, 0xF102005F, 0x54000648},
                      GuestFunction::Memmove},
};

} // namespace

std::span<const GuestFunctionSite> GuestFunctionSites(std::string_view build_id) {
    const auto first = std::find_if(Sites.begin(), Sites.end(),
                                    [&](const auto& site) { return site.build_id == build_id; });
    const auto last = std::find_if(first, Sites.end(),
                                   [&](const auto& site) { return site.build_id != build_id; });
    return {first, last};
}

} // namespace Core
