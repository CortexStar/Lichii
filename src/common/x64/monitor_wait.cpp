// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#include "common/cpu_features.h"
#include "common/x64/monitor_wait.h"
#include "common/x64/rdtsc.h"

#if defined(__clang__) || defined(__GNUC__)
#define MONITOR_WAIT_TARGET(features) __attribute__((target(features)))
#else
#define MONITOR_WAIT_TARGET(features)
#endif

namespace Common::X64 {

namespace {

bool Holds(const void* address, u64 expected, u32 size) {
    if (size == sizeof(u64)) {
        return *static_cast<const volatile u64*>(address) == expected;
    }
    return *static_cast<const volatile u32*>(address) == static_cast<u32>(expected);
}

MONITOR_WAIT_TARGET("mwaitx")
void WaitMwaitx(const void* address, u64 expected, u32 size, u32 ticks) {
    _mm_monitorx(const_cast<void*>(address), 0, 0);
    if (Holds(address, expected, size)) {
        // ECX bit 1 enables the EBX timer (TSC ticks); EAX 0xF0 asks for no C-state, so the
        // wake-up is as fast as the PAUSE loop it replaces.
        _mm_mwaitx(1U << 1, 0xF0U, ticks);
    }
}

MONITOR_WAIT_TARGET("waitpkg")
void WaitUmwait(const void* address, u64 expected, u32 size, u32 ticks) {
    _umonitor(const_cast<void*>(address));
    if (Holds(address, expected, size)) {
        // Control 1 is C0.1, the state with the faster wake-up; the deadline is absolute.
        _umwait(1U, FencedRDTSC() + ticks);
    }
}

} // namespace

bool HasMonitorWait() {
    return g_cpu_caps.monitorx || g_cpu_caps.waitpkg;
}

void MonitorWait(const void* address, u64 expected, u32 size, u32 max_tsc_ticks) {
    if (g_cpu_caps.monitorx) {
        WaitMwaitx(address, expected, size, max_tsc_ticks);
    } else if (g_cpu_caps.waitpkg) {
        WaitUmwait(address, expected, size, max_tsc_ticks);
    }
}

} // namespace Common::X64
