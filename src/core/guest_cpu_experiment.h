// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENT (do not merge): EDEN_NX2_GUEST_CPU_OFF=cntpct,spin,idle,arb,hostfn,svcwait turns the named
// guest-CPU changes off for an A/B in one build.

#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

namespace Core {

inline bool GuestCpuExperimentOff(std::string_view name) {
    static const std::string off = [] {
        const char* value = std::getenv("EDEN_NX2_GUEST_CPU_OFF");
        return std::string{value != nullptr ? value : ""};
    }();
    return off.find(name) != std::string::npos;
}

} // namespace Core
