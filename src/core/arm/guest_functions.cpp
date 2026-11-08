// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "core/arm/guest_functions.h"

namespace Core {

namespace {

// The "sdk" module (libc, nn::os) of Ocarina of Time version 131072.
constexpr std::string_view Sdk1643 = "1643A838814F9C10C696F930ED681E7FA3FB9AF8";
// Ocarina of Time version 131072, "main".
constexpr std::string_view OotMain200 = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";

constexpr std::array Sites{
    // memcpy: a branch to __memcpy_aarch64 through the PLT.
    GuestFunctionSite{Sdk1643, 0x78F020, {0x1403B778, 0x00000000, 0x00000000, 0x00000000},
                      GuestFunction::Memmove},
    GuestFunctionSite{Sdk1643, 0x78F040, {0xEB01001F, 0x54000BA0, 0x8B020028, 0xEB00011F},
                      GuestFunction::Memmove},
    GuestFunctionSite{Sdk1643, 0x78F300, {0xB40006C2, 0x8B020008, 0x39000001, 0x381FF101},
                      GuestFunction::Memset},
    GuestFunctionSite{Sdk1643, 0x7B8CC0, {0x1E26002C, 0x531F798A, 0x3400030A, 0x12007988},
                      GuestFunction::Fmodf},
    // __memcpy_aarch64 and __memcpy_aarch64_simd (Arm optimized-routines): both copy
    // overlapping buffers as memmove does (backwards when dst - src < n).
    GuestFunctionSite{Sdk1643, 0x7BCDC0, {0x8B020024, 0x8B020005, 0xF102005F, 0x540007A8},
                      GuestFunction::Memmove},
    GuestFunctionSite{Sdk1643, 0x7BD000, {0x8B020024, 0x8B020005, 0xF102005F, 0x54000648},
                      GuestFunction::Memmove},
    // MurmurHash3_x86_32 of a NUL-terminated name, seed in w1.
    GuestFunctionSite{OotMain200, 0x1739E0, {0x3940000C, 0x3400086C, 0x5285AA2A, 0x5286B268},
                      GuestFunction::MurmurHash3String},
};

constexpr u32 SignBit = 0x8000'0000;
constexpr u32 DefaultNaN = 0x7FC0'0000;
constexpr u32 FpsrIoc = 1U << 0;
constexpr u32 FpsrIdc = 1U << 7;
constexpr u32 FpcrFz = 1U << 24;
constexpr u32 FpcrDn = 1U << 25;

constexpr bool IsNaN(u32 v) {
    return (v & ~SignBit) > 0x7F80'0000;
}
constexpr bool IsSignalingNaN(u32 v) {
    return IsNaN(v) && (v & 0x0040'0000) == 0;
}
constexpr bool IsDenormal(u32 v) {
    return (v & 0x7F80'0000) == 0 && (v & 0x007F'FFFF) != 0;
}
constexpr bool IsInfinity(u32 v) {
    return (v & ~SignBit) == 0x7F80'0000;
}

/// FPUnpack: with FZ a denormal input reads as zero and raises IDC.
constexpr u32 Unpack(u32 v, u32 fpcr, u32& fpsr) {
    if ((fpcr & FpcrFz) != 0 && IsDenormal(v)) {
        fpsr |= FpsrIdc;
        return v & SignBit;
    }
    return v;
}

/// FPProcessNaN for an input NaN.
constexpr u32 ProcessNaN(u32 v, u32 fpcr, u32& fpsr) {
    if (IsSignalingNaN(v)) {
        fpsr |= FpsrIoc;
        v |= 0x0040'0000;
    }
    return (fpcr & FpcrDn) != 0 ? DefaultNaN : v;
}

/// (x*y)/(x*y) for the inputs that reach it: y = +-0, y NaN, x infinite or NaN. The product is
/// then a NaN, an infinity or a zero, never a finite non-zero, and the quotient of an infinity
/// or a zero by itself is invalid.
constexpr u32 SpecialQuotient(u32 x, u32 y, u32 fpcr, u32& fpsr) {
    const u32 a = Unpack(x, fpcr, fpsr);
    const u32 b = Unpack(y, fpcr, fpsr);
    u32 product;
    if (IsSignalingNaN(a)) {
        product = ProcessNaN(a, fpcr, fpsr);
    } else if (IsSignalingNaN(b)) {
        product = ProcessNaN(b, fpcr, fpsr);
    } else if (IsNaN(a)) {
        product = ProcessNaN(a, fpcr, fpsr);
    } else if (IsNaN(b)) {
        product = ProcessNaN(b, fpcr, fpsr);
    } else if ((IsInfinity(a) && (b & ~SignBit) == 0) || (IsInfinity(b) && (a & ~SignBit) == 0)) {
        fpsr |= FpsrIoc;
        product = DefaultNaN;
    } else if (IsInfinity(a) || IsInfinity(b)) {
        product = ((a ^ b) & SignBit) | 0x7F80'0000;
    } else {
        product = (a ^ b) & SignBit; // one of them is zero here
    }
    if (IsNaN(product)) {
        return ProcessNaN(product, fpcr, fpsr); // quiet already: no flag
    }
    fpsr |= FpsrIoc; // inf/inf or 0/0
    return DefaultNaN;
}

/// 0*x for a finite x (the zero and |x| <= |y| results): a zero with x's sign, and IDC when FZ
/// flushes a denormal x.
constexpr u32 ZeroTimes(u32 x, u32 fpcr, u32& fpsr) {
    Unpack(x, fpcr, fpsr);
    return x & SignBit;
}

} // namespace

std::span<const GuestFunctionSite> GuestFunctionSites(std::string_view build_id) {
    const auto first = std::find_if(Sites.begin(), Sites.end(),
                                    [&](const auto& site) { return site.build_id == build_id; });
    const auto last = std::find_if(first, Sites.end(),
                                   [&](const auto& site) { return site.build_id != build_id; });
    return {first, last};
}

FmodfResult Fmodf(u32 x, u32 y, u32 fpcr) {
    u32 fpsr = 0;
    int ex = static_cast<int>((x >> 23) & 0xFF);
    int ey = static_cast<int>((y >> 23) & 0xFF);
    const u32 sx = x & SignBit;
    u32 uxi = x;
    u32 uyi = y;

    if ((uyi << 1) == 0 || IsNaN(uyi) || ex == 0xFF) {
        const u32 value = SpecialQuotient(x, y, fpcr, fpsr);
        return {value, fpsr};
    }
    if ((uxi << 1) <= (uyi << 1)) {
        // fmul s1, s0, #0 runs on both paths; fcsel picks it only when |x| == |y|.
        const u32 zero = ZeroTimes(x, fpcr, fpsr);
        return {(uxi << 1) == (uyi << 1) ? zero : x, fpsr};
    }

    // Normalize x and y.
    if (ex == 0) {
        for (u32 i = uxi << 9; (i >> 31) == 0; ex--, i <<= 1) {
        }
        uxi <<= -ex + 1;
    } else {
        uxi &= ~0U >> 9;
        uxi |= 1U << 23;
    }
    if (ey == 0) {
        for (u32 i = uyi << 9; (i >> 31) == 0; ey--, i <<= 1) {
        }
        uyi <<= -ey + 1;
    } else {
        uyi &= ~0U >> 9;
        uyi |= 1U << 23;
    }

    // musl subtracts and shifts once per exponent step (ex - ey steps): the remainder of
    // uxi * 2^(ex - ey) by uyi. Both are below 2^24, so 39 steps fit a 64-bit shift.
    u64 remainder = uxi % uyi;
    for (int steps = ex - ey; steps > 0;) {
        const int shift = std::min(steps, 39);
        remainder = (remainder << shift) % uyi;
        steps -= shift;
    }
    if (remainder == 0) {
        return {ZeroTimes(x, fpcr, fpsr), fpsr};
    }
    uxi = static_cast<u32>(remainder);
    ex = ey;
    for (; (uxi >> 23) == 0; uxi <<= 1, ex--) {
    }

    // Scale the result up.
    if (ex > 0) {
        uxi -= 1U << 23;
        uxi |= static_cast<u32>(ex) << 23;
    } else {
        uxi >>= -ex + 1;
    }
    return {uxi | sx, fpsr};
}

} // namespace Core
