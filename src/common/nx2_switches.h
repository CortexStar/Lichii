// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EXPERIMENT (do not merge): the A/B switches of the Ocarina of Time work, flippable while the
// game runs. Each starts from its environment variable, parsed as its read site always did, so a
// launch with the variable set behaves as before. The debug server's `switch` command requests a
// new value; it takes effect at the next frame (Latch, from the rasterizer's TickFrame), so a flip
// never lands inside per-frame work (an upload batch, staged dynamic state). A read is one relaxed
// atomic load.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Common::Nx2Switches {

/// Named after what a set switch does; the comment gives the variable and the value that sets it.
enum class Switch : u32 {
    NoPark,              ///< EDEN_NX2_EXP_NO_PARK=1
    NoFenceQuiet,        ///< EDEN_NX2_EXP_NO_FENCE_QUIET=1
    FenceFlushTwice,     ///< EDEN_NX2_EXP_FENCE_FLUSH_TWICE=1
    ColdAlways,          ///< EDEN_NX2_EXP_COLD_ALWAYS=1
    NoEarlyRead,         ///< EDEN_NX2_EXP_NO_EARLY_READ=1
    NoUploadBatch,       ///< EDEN_NX2_EXP_NO_UPLOAD_BATCH=1
    GuestCpuOff,         ///< EDEN_NX2_GUEST_CPU_OFF=<names>: a GuestCpuOffBits mask
    DispatchBarriersAll, ///< EDEN_NX2_EXP_DISPATCH_BARRIERS=all
    DynStateUnbatched,   ///< EDEN_NX2_DYNSTATE_BATCH=0
    PresentCadenceOff,   ///< EDEN_NX2_PRESENT_CADENCE=0
    FgHudlessOff,        ///< EDEN_NX2_EXP_FG_HUDLESS=0
    HeavyDispatch,       ///< EDEN_NX2_EXP_HEAVY_DISPATCH=1
    KeepPresentFilters,  ///< EDEN_NX2_EXP_PRESENT_FILTERS=1
    NoClaim,             ///< EDEN_NX2_EXP_NO_CLAIM (set at all)
    NoBuilderBoost,      ///< EDEN_NX2_EXP_NO_BUILDER_BOOST (set at all)
    Evict9000,           ///< EDEN_NX2_EXP_EVICT_9000 (set at all)
    NoGplRelink,         ///< EDEN_NX2_EXP_NO_GPL_RELINK (set at all)
    Count,
};

/// Parts of EDEN_NX2_GUEST_CPU_OFF. Cntpct and Hostfn change emitted code: they take effect only
/// at launch (AtLaunch), the rest also at run time (Get).
enum GuestCpuOffBits : u32 {
    Cntpct = 1u << 0,
    Spin = 1u << 1,
    Idle = 1u << 2,
    Arb = 1u << 3,
    Hostfn = 1u << 4,
    Svcwait = 1u << 5,
};

/// The value now: 1 when the switch is set (a GuestCpuOffBits mask for GuestCpuOff), else 0.
[[nodiscard]] u32 Get(Switch which) noexcept;

/// The value the process started with, for parts that only a restart can change.
[[nodiscard]] u32 AtLaunch(Switch which) noexcept;

/// The environment variable of a switch.
[[nodiscard]] std::string_view Name(Switch which) noexcept;

/// The switch of an environment variable name, if it is one.
[[nodiscard]] std::optional<Switch> Find(std::string_view name) noexcept;

/// The value a text gives, parsed as the read site parses its variable. No text: unset.
[[nodiscard]] u32 Parse(Switch which, std::optional<std::string_view> text);

/// Asks for the switch to hold the text (no text: as if the variable were unset) from the next
/// Latch on. @return The value it will have.
u32 Request(Switch which, std::optional<std::string_view> text);

/// The text a switch holds or was asked to hold; no text when unset.
[[nodiscard]] std::optional<std::string> Text(Switch which);

struct Change {
    Switch which;
    u32 from;
    u32 to;
};

/// Applies the requested values and logs each change with the frame. Called once a frame; costs
/// one atomic load when nothing was requested.
std::vector<Change> Latch(u64 frame);

} // namespace Common::Nx2Switches
