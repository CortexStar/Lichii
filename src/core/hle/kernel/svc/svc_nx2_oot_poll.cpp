// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdlib>

#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc/svc_nx2_oot_poll.h"

namespace Kernel::Svc {
namespace {

// Ocarina of Time (main build E0F3CCFA...): the resource request thread (main+0xF7A0, state 3)
// walks its pending requests (a name lookup through the query engine at main+0x58EEA0, a mutex and
// status checks per request) and, if any is not ready yet, starts the walk again at once with no
// wait. It only ever polls, so sleeping before the next walk keeps every outcome and just lowers
// the rate; the game's own idle wait in the same loop is 16 ms.
constexpr u64 kOotProgramId = 0x0400486024DEC000ULL;
constexpr u32 kPollSvc = 0xF3;

// EDEN_NX2_OOT_POLL_BACKOFF: a number above 1 is the sleep in us (at most 16000), anything else
// 500 us. "0" turns the patch off (patch_manager.cpp), and then this SVC never runs.
s64 BackoffNs() {
    static const s64 ns = [] {
        const char* value = std::getenv("EDEN_NX2_OOT_POLL_BACKOFF");
        s64 us = 500;
        if (value != nullptr && value[0] != 0) {
            const long long parsed = std::strtoll(value, nullptr, 10);
            if (parsed > 1) {
                us = parsed < 16000 ? parsed : 16000;
            }
        }
        return us * 1000;
    }();
    return ns;
}

} // Anonymous namespace

bool CallNx2OotPollBackoff(Core::System& system, u32 imm) {
    if (imm != kPollSvc) {
        return false;
    }
    auto& process = GetCurrentProcess(system.Kernel());
    if (!process.Is64Bit() || process.GetProgramId() != kOotProgramId) {
        return false;
    }
    static std::atomic<u64> calls{};
    const u64 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 100000 == 0) {
        LOG_INFO(Kernel_SVC, "OoT resource poll backoff: {} waits of {} us", n,
                 BackoffNs() / 1000);
    }
    SleepThread(system, BackoffNs());
    return true;
}

} // namespace Kernel::Svc
