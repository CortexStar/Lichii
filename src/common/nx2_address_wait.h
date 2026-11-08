// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>

#include "common/common_types.h"

/// Address waits for hot locks. On Windows, libstdc++'s std::atomic::wait goes through a
/// winpthreads mutex and condition variable shared by every address that hashes to the same slot:
/// a semaphore round trip per wait and per wake, wakers block on the slot's mutex, and every
/// sleeper of the slot wakes. WaitOnAddress wakes only the word's sleepers and costs no syscall to
/// wake a word nobody sleeps on. Elsewhere (and if the API is missing) std::atomic::wait is used.
namespace Common::Nx2AddressWait {

/// Sleeps while word holds expected; may return early.
void Wait(std::atomic<u32>& word, u32 expected);
void WakeOne(std::atomic<u32>& word);
void WakeAll(std::atomic<u32>& word);

/// Returns once word no longer holds expected. For the first `spin` it stays on the CPU: PAUSE
/// polling for the first 2 us (handoffs that land that soon are seen ~80 ns sooner than an MWAITX
/// wakes up), then MONITORX/MWAITX (or UMONITOR/UMWAIT) on the word's line, which wakes on the
/// waker's store in ~0.2 us while the SMT sibling gets the core. After `spin` it sleeps in Wait
/// (an OS wake costs ~5-10 us). Wakers store the new value, then call WakeOne/WakeAll.
void WaitSpinning(std::atomic<u32>& word, u32 expected, std::chrono::nanoseconds spin);

} // namespace Common::Nx2AddressWait
