// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/load_timing.h"

#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64) ||                                 \
    defined(ARCHITECTURE_riscv64) || defined(ARCHITECTURE_loongarch64)
#include "dynarmic/interface/A64/a64.h"
#define LOAD_TIMING_HAS_DYNARMIC 1
#endif

namespace Core::LoadTiming {

namespace {

std::atomic<u64> file_reads{0};
std::atomic<u64> file_bytes{0};
std::atomic<u64> file_ns{0};
std::atomic<u64> service_calls{0};
std::atomic<u64> service_ns{0};

struct State {
    std::mutex mutex;
    Clock::time_point boot_start{Clock::now()};
    Counters boot_counters{};
    bool first_frame_seen{};
    LoadWindowTracker tracker;
};

State& GetState() {
    static State state;
    return state;
}

u64 ToNs(Clock::duration duration) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

double Ms(u64 ns) {
    return static_cast<double>(ns) / 1e6;
}

double SecondsSince(Clock::time_point origin, Clock::time_point point) {
    return std::chrono::duration<double>(point - origin).count();
}

std::string Describe(const Counters& work) {
    std::string text = fmt::format(
        "host file reads {} ({:.1f} MiB in {:.0f} ms), service calls {} ({:.0f} ms in handlers)",
        work.file_reads, static_cast<double>(work.file_bytes) / (1024.0 * 1024.0), Ms(work.file_ns),
        work.service_calls, Ms(work.service_ns));
    if (work.jit_blocks != 0 || work.jit_cache_clears != 0) {
        text += fmt::format(", JIT {} blocks in {:.0f} ms, {} code cache clears", work.jit_blocks,
                            Ms(work.jit_ns), work.jit_cache_clears);
    }
    return text;
}

} // Anonymous namespace

Counters Counters::operator-(const Counters& rhs) const {
    return {
        .file_reads = file_reads - rhs.file_reads,
        .file_bytes = file_bytes - rhs.file_bytes,
        .file_ns = file_ns - rhs.file_ns,
        .service_calls = service_calls - rhs.service_calls,
        .service_ns = service_ns - rhs.service_ns,
        .jit_blocks = jit_blocks - rhs.jit_blocks,
        .jit_ns = jit_ns - rhs.jit_ns,
        .jit_cache_clears = jit_cache_clears - rhs.jit_cache_clears,
    };
}

void CountFileRead(u64 bytes, Clock::duration host_time) {
    file_reads.fetch_add(1, std::memory_order_relaxed);
    file_bytes.fetch_add(bytes, std::memory_order_relaxed);
    file_ns.fetch_add(ToNs(host_time), std::memory_order_relaxed);
}

void CountServiceCall(Clock::duration handler_time) {
    service_calls.fetch_add(1, std::memory_order_relaxed);
    service_ns.fetch_add(ToNs(handler_time), std::memory_order_relaxed);
}

Counters ReadCounters() {
    Counters counters{
        .file_reads = file_reads.load(std::memory_order_relaxed),
        .file_bytes = file_bytes.load(std::memory_order_relaxed),
        .file_ns = file_ns.load(std::memory_order_relaxed),
        .service_calls = service_calls.load(std::memory_order_relaxed),
        .service_ns = service_ns.load(std::memory_order_relaxed),
    };
#ifdef LOAD_TIMING_HAS_DYNARMIC
    const auto& jit = Dynarmic::A64::compile_statistics;
    counters.jit_blocks = jit.blocks.load(std::memory_order_relaxed);
    counters.jit_ns = jit.nanoseconds.load(std::memory_order_relaxed);
    counters.jit_cache_clears = jit.cache_clears.load(std::memory_order_relaxed);
#endif
    return counters;
}

std::optional<LoadWindow> LoadWindowTracker::OnFrame(Clock::time_point now,
                                                     const Counters& counters) {
    std::optional<LoadWindow> finished;
    if (has_frame) {
        const bool slow = now - last_frame >= SlowFrame;
        if (!in_load) {
            if (slow) {
                in_load = true;
                regular_run = 0;
                window = {.begin = last_frame, .end = now, .frames = 1};
                begin_counters = last_counters;
                window.work = counters - begin_counters;
            }
        } else {
            ++window.frames;
            if (slow) {
                regular_run = 0;
                window.end = now;
                window.work = counters - begin_counters;
            } else if (++regular_run >= RegularFrames) {
                in_load = false;
                // The regular frames after the last slow one are not part of the load.
                window.frames -= regular_run;
                if (window.end - window.begin >= MinimumLoad) {
                    finished = window;
                }
            }
        }
    }
    has_frame = true;
    last_frame = now;
    last_counters = counters;
    return finished;
}

void BeginBoot() {
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.boot_start = Clock::now();
    state.boot_counters = ReadCounters();
    state.first_frame_seen = false;
    state.tracker = {};
}

void LogPhase(std::string_view name, Clock::time_point start) {
    LOG_INFO(Core, "Load phase {}: {:.0f} ms", name, Ms(ToNs(Clock::now() - start)));
}

void OnGuestFrame() {
    const auto now = Clock::now();
    const Counters counters = ReadCounters();
    auto& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.first_frame_seen) {
        state.first_frame_seen = true;
        LOG_INFO(Core, "Load phase boot to first guest frame: {:.0f} ms; {}",
                 SecondsSince(state.boot_start, now) * 1000.0,
                 Describe(counters - state.boot_counters));
    }
    if (const auto load = state.tracker.OnFrame(now, counters)) {
        const double seconds = SecondsSince(load->begin, load->end);
        LOG_INFO(Core, "Load: {:.1f} s from {:.1f} s after boot, {} frames ({:.1f} fps); {}",
                 seconds, SecondsSince(state.boot_start, load->begin), load->frames,
                 static_cast<double>(load->frames) / seconds, Describe(load->work));
    }
}

} // namespace Core::LoadTiming
