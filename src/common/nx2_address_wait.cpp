// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/nx2_address_wait.h"

#include <algorithm>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#include "common/cpu_features.h"
#include "common/x64/monitor_wait.h"
#endif

namespace Common::Nx2AddressWait {

namespace {

#ifdef _WIN32
using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressFn = VOID(WINAPI*)(PVOID);

struct AddressApi {
    WaitOnAddressFn wait{};
    WakeByAddressFn wake_one{};
    WakeByAddressFn wake_all{};
};

const AddressApi& Api() {
    static const AddressApi api = [] {
        AddressApi result{};
        HMODULE module = GetModuleHandleA("KernelBase.dll");
        if (module == nullptr) {
            module = LoadLibraryA("api-ms-win-core-synch-l1-2-0.dll");
        }
        if (module == nullptr) {
            return result;
        }
        result.wait = reinterpret_cast<WaitOnAddressFn>(GetProcAddress(module, "WaitOnAddress"));
        result.wake_one =
            reinterpret_cast<WakeByAddressFn>(GetProcAddress(module, "WakeByAddressSingle"));
        result.wake_all =
            reinterpret_cast<WakeByAddressFn>(GetProcAddress(module, "WakeByAddressAll"));
        if (result.wait == nullptr || result.wake_one == nullptr || result.wake_all == nullptr) {
            return AddressApi{};
        }
        return result;
    }();
    return api;
}
#endif

} // Anonymous namespace

void Wait(std::atomic<u32>& word, u32 expected) {
#ifdef _WIN32
    if (const AddressApi& api = Api(); api.wait != nullptr) {
        api.wait(static_cast<void*>(&word), &expected, sizeof(u32), INFINITE);
        return;
    }
#endif
    word.wait(expected, std::memory_order_relaxed);
}

void WakeOne(std::atomic<u32>& word) {
#ifdef _WIN32
    if (const AddressApi& api = Api(); api.wake_one != nullptr) {
        api.wake_one(static_cast<void*>(&word));
        return;
    }
#endif
    word.notify_one();
}

void WakeAll(std::atomic<u32>& word) {
#ifdef _WIN32
    if (const AddressApi& api = Api(); api.wake_all != nullptr) {
        api.wake_all(static_cast<void*>(&word));
        return;
    }
#endif
    word.notify_all();
}

void WaitSpinning(std::atomic<u32>& word, u32 expected, std::chrono::nanoseconds spin) {
    if (spin > std::chrono::nanoseconds::zero()) {
#if defined(_M_X64) || defined(__x86_64__)
        if (Common::g_wall_clock.IsNative() && Common::X64::HasMonitorWait()) {
            const u64 budget = Common::g_wall_clock.NsToTicks(spin);
            const u64 poll = Common::g_wall_clock.NsToTicks(std::chrono::microseconds{2});
            const u64 start = __rdtsc();
            while (word.load(std::memory_order_acquire) == expected) {
                const u64 elapsed = __rdtsc() - start;
                if (elapsed >= budget) {
                    break;
                }
                if (elapsed < poll) {
                    _mm_pause();
                    _mm_pause();
                    continue;
                }
                Common::X64::MonitorWait(&word, expected, sizeof(u32),
                                         static_cast<u32>((std::min)(budget - elapsed,
                                                                     u64{0xFFFF'FFFF})));
            }
        } else
#endif
        {
            const auto deadline = std::chrono::steady_clock::now() + spin;
            while (word.load(std::memory_order_acquire) == expected &&
                   std::chrono::steady_clock::now() < deadline) {
#if defined(_M_X64) || defined(__x86_64__)
                for (int i = 0; i < 8; ++i) {
                    _mm_pause();
                }
#else
                std::this_thread::yield();
#endif
            }
        }
    }
    while (word.load(std::memory_order_acquire) == expected) {
        Wait(word, expected);
    }
}

} // namespace Common::Nx2AddressWait
