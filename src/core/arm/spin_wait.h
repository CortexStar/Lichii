// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>

#include "common/common_types.h"

namespace Core {

/// What to do for one guest YIELD (or WFE) hint.
enum class SpinAction : u8 {
    Pause,      ///< Two PAUSEs: a short spin that usually ends within a few iterations.
    YieldGuestCore, ///< Hand the guest core to another ready thread of the same priority.
    YieldHostCpu, ///< Give up the host CPU for a moment (the lock holder's host thread may be waiting for it).
    Wait,       ///< Wait for a write to the line the loop polls (MonitorWait), bounded.
};

/// Classifies YIELD hints into spin episodes. A YIELD less than `gap` ticks after the previous one
/// continues the episode. Its first PauseYields hints pause, as before; past them the core waits
/// for the polled line instead, gives the guest scheduler a chance every GuestYieldPeriod hints
/// (real HOS never preempts a spinning thread for an equal-priority one on its own core, so a
/// guest yield there only reorders threads the way a preemption could), and the host scheduler
/// every HostYieldPeriod hints.
class SpinEpisode {
public:
    static constexpr u32 PauseYields = 64;
    static constexpr u32 GuestYieldPeriod = 256;
    static constexpr u32 HostYieldPeriod = 1024;

    constexpr SpinAction OnYield(u64 now, u64 gap) {
        if (now - m_last > gap) {
            m_count = 0;
        }
        m_last = now;
        const u32 n = m_count++;
        if (n < PauseYields) {
            return SpinAction::Pause;
        }
        const u32 k = n - PauseYields;
        if (k % GuestYieldPeriod == 0) {
            return SpinAction::YieldGuestCore;
        }
        if (k % HostYieldPeriod == HostYieldPeriod - 1) {
            return SpinAction::YieldHostCpu;
        }
        return SpinAction::Wait;
    }

private:
    u64 m_last{~u64{0} >> 1};
    u32 m_count{};
};

/// The memory word a spin loop polls right after its YIELD: `base` is a register number (31 is
/// SP), the address is base + offset, aligned down to `size` bytes (4 or 8).
struct SpinWatch {
    u8 base{};
    s64 offset{};
    u8 size{};

    constexpr bool operator==(const SpinWatch&) const = default;
};

/// Decodes the instructions after a YIELD: any number of `add xd, xn, #imm` and barriers, then
/// a load, load-acquire, exclusive load, compare-and-swap or LSE atomic. Returns the word it
/// accesses, or nothing if the loop does not poll memory there (a backoff counter, a branch).
constexpr std::optional<SpinWatch> DecodeSpinWatch(std::span<const u32> after_yield) {
    // Registers that hold base + offset of another register after the adds seen so far.
    struct Alias {
        u8 base;
        s64 offset;
    };
    std::optional<Alias> alias[32]{};
    const auto resolve = [&](u32 reg, s64 offset) -> SpinWatch {
        if (reg != 31 && alias[reg]) {
            return {alias[reg]->base, alias[reg]->offset + offset, 0};
        }
        return {static_cast<u8>(reg), offset, 0};
    };
    const auto watch = [](SpinWatch w, u32 size_log2) -> std::optional<SpinWatch> {
        w.size = size_log2 == 3 ? 8 : 4;
        return w;
    };
    for (const u32 insn : after_yield) {
        const u32 rn = (insn >> 5) & 31;
        const u32 rd = insn & 31;
        const u32 size_log2 = insn >> 30;
        if ((insn & 0xFF800000) == 0x91000000) {
            // add xd, xn|sp, #imm{, lsl #12}
            const s64 imm = s64((insn >> 10) & 0xFFF) << (((insn >> 22) & 1) ? 12 : 0);
            if (rd == 31) {
                return std::nullopt;
            }
            const SpinWatch source = resolve(rn, imm);
            alias[rd] = Alias{source.base, source.offset};
            continue;
        }
        if ((insn & 0xFFFFF01F) == 0xD503301F) {
            // dmb/dsb/isb (CRm any)
            continue;
        }
        if ((insn & 0x3F000000) == 0x08000000) {
            // Load/store exclusive, load-acquire/store-release, CAS: address [xn]
            if ((insn & (1U << 22)) == 0 && (insn & (1U << 21)) == 0) {
                return std::nullopt; // stxr, stlxr, stlr: L and o1 clear, nothing is read
            }
            return watch(resolve(rn, 0), size_log2);
        }
        if ((insn & 0x3FFFFC00) == 0x38BFC000) {
            // ldapr
            return watch(resolve(rn, 0), size_log2);
        }
        if ((insn & 0x3F200C00) == 0x38200000) {
            // LSE atomic memory operation (ldadd, swp, ...)
            return watch(resolve(rn, 0), size_log2);
        }
        if ((insn & 0x3FC00000) == 0x39400000) {
            // ldr (unsigned immediate offset)
            return watch(resolve(rn, s64((insn >> 10) & 0xFFF) << size_log2), size_log2);
        }
        if ((insn & 0x3FE00C00) == 0x38400000) {
            // ldur
            const s64 imm9 = s64(((insn >> 12) & 0x1FF) ^ 0x100) - 0x100;
            return watch(resolve(rn, imm9), size_log2);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace Core
