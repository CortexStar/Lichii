// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>

#include "common/logging.h"
#include "video_core/guest_frame_timing.h"

namespace VideoCore::GuestFrameTiming {

namespace {

constexpr size_t CAPACITY = 1024;

u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

struct State {
    std::atomic<u64> id{};
    std::atomic<u64> start_ns{};
    std::mutex mutex;
    std::array<double, CAPACITY> samples{};
    u64 count{};
    u64 since_log{};
};

State& GetState() {
    static State state;
    return state;
}

} // Anonymous namespace

Mark BeginFrame() {
    State& state = GetState();
    const u64 now = NowNs();
    state.start_ns.store(now, std::memory_order_relaxed);
    const u64 id = state.id.fetch_add(1, std::memory_order_acq_rel) + 1;
    return {id, now};
}

Mark Current() {
    const State& state = GetState();
    return {state.id.load(std::memory_order_acquire),
            state.start_ns.load(std::memory_order_relaxed)};
}

void RecordPresent(const Mark& mark) {
    if (mark.id == 0 || mark.start_ns == 0) {
        return;
    }
    const u64 now = NowNs();
    if (now < mark.start_ns) {
        return;
    }
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.samples[state.count % CAPACITY] = static_cast<double>(now - mark.start_ns) / 1e6;
    ++state.count;
    if (++state.since_log == 600) {
        state.since_log = 0;
        const LatencyStats stats = Summarize(state.samples, state.count);
        LOG_INFO(Render_Vulkan,
                 "Guest frame start to host present (last {} frames): mean {:.2f} ms, p50 {:.2f} "
                 "ms, p99 {:.2f} ms, max {:.2f} ms",
                 stats.samples, stats.mean_ms, stats.p50_ms, stats.p99_ms, stats.max_ms);
    }
}

LatencyStats Recent() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    return Summarize(state.samples, state.count);
}

LatencyStats Summarize(std::array<double, 1024> samples, u64 count) {
    const size_t n = static_cast<size_t>(std::min<u64>(count, CAPACITY));
    if (n == 0) {
        return {};
    }
    std::sort(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(n));
    const auto at = [&](double fraction) {
        return samples[static_cast<size_t>(fraction * static_cast<double>(n - 1) + 0.5)];
    };
    return {
        .samples = n,
        .mean_ms = std::accumulate(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(n),
                                   0.0) /
                   static_cast<double>(n),
        .p50_ms = at(0.5),
        .p99_ms = at(0.99),
        .max_ms = samples[n - 1],
    };
}

} // namespace VideoCore::GuestFrameTiming
