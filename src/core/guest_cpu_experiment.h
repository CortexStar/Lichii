// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENT (do not merge): EDEN_NX2_GUEST_CPU_OFF=cntpct,spin,idle,arb,hostfn,svcwait turns the named
// guest-CPU changes off for an A/B in one build.

#pragma once

#include "common/nx2_switches.h"

namespace Core {

/// Whether the guest-CPU change `bit` (a Common::Nx2Switches::GuestCpuOffBits) is turned off.
/// Cntpct and Hostfn change emitted code: they keep the value the process started with. The others
/// are read per call and follow the debug server's `switch` command.
inline bool GuestCpuExperimentOff(u32 bit) {
    namespace S = Common::Nx2Switches;
    const bool emitted = (bit & (S::Cntpct | S::Hostfn)) != 0;
    const u32 off = emitted ? S::AtLaunch(S::Switch::GuestCpuOff) : S::Get(S::Switch::GuestCpuOff);
    return (off & bit) != 0;
}

} // namespace Core
