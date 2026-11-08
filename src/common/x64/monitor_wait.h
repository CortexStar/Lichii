// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Common::X64 {

/// Whether MonitorWait can wait: MONITORX/MWAITX (AMD) or UMONITOR/UMWAIT (Intel WAITPKG).
bool HasMonitorWait();

/// Waits in user mode until the cache line holding `address` is written, `max_tsc_ticks` TSC
/// ticks pass, or an interrupt arrives, and returns at once if the `size` (4 or 8) bytes at
/// `address` no longer hold `expected` once the monitor is armed (no lost wake-up). The thread
/// stays scheduled but issues no instructions, so its SMT sibling gets the core and the wake-up
/// costs no OS call. Returns at once if HasMonitorWait() is false.
void MonitorWait(const void* address, u64 expected, u32 size, u32 max_tsc_ticks);

} // namespace Common::X64
