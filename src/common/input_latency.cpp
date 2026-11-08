// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <mutex>
#include <numeric>

#include "common/input_latency.h"
#include "common/logging.h"

namespace Common::InputLatency {

namespace {

u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

struct State {
    Tracker tracker;
    std::atomic<u64> current_frame_input{};

    std::mutex mutex;
    std::array<double, MaxSamples> samples_ms{};
    u64 count{};
    u64 last_log_ns{};
};

State& GetState() {
    static State state;
    return state;
}

} // Anonymous namespace

void Tracker::Changed(u64 ns) {
    u64 expected = 0;
    pending.compare_exchange_strong(expected, ns, std::memory_order_acq_rel);
}

u64 Tracker::FrameStart() {
    return pending.exchange(0, std::memory_order_acq_rel);
}

std::optional<u64> Tracker::Present(u64 input_ns, u64 now_ns) {
    if (input_ns == 0 || input_ns == last_presented || now_ns < input_ns) {
        return std::nullopt;
    }
    last_presented = input_ns;
    return now_ns - input_ns;
}

Stats Summarize(std::array<double, MaxSamples> samples_ms, u64 count) {
    const std::size_t n = static_cast<std::size_t>((std::min<u64>)(count, MaxSamples));
    if (n == 0) {
        return {};
    }
    const auto end = samples_ms.begin() + static_cast<std::ptrdiff_t>(n);
    std::sort(samples_ms.begin(), end);
    const auto percentile = [&](double fraction) {
        const double index = fraction * static_cast<double>(n - 1);
        const std::size_t low = static_cast<std::size_t>(index);
        const std::size_t high = (std::min)(low + 1, n - 1);
        return samples_ms[low] +
               (samples_ms[high] - samples_ms[low]) * (index - static_cast<double>(low));
    };
    return {
        .samples = n,
        .mean_ms = std::accumulate(samples_ms.begin(), end, 0.0) / static_cast<double>(n),
        .p50_ms = percentile(0.5),
        .p99_ms = percentile(0.99),
        .max_ms = samples_ms[n - 1],
    };
}

void Changed() {
    GetState().tracker.Changed(NowNs());
}

void FrameStart() {
    State& state = GetState();
    state.current_frame_input.store(state.tracker.FrameStart(), std::memory_order_release);
}

u64 CurrentFrameInput() {
    return GetState().current_frame_input.load(std::memory_order_acquire);
}

void RecordPresent(u64 input_ns) {
    State& state = GetState();
    const u64 now = NowNs();
    const auto latency = state.tracker.Present(input_ns, now);
    if (!latency) {
        return;
    }
    std::array<double, MaxSamples> samples;
    u64 count;
    {
        std::scoped_lock lock{state.mutex};
        state.samples_ms[state.count % MaxSamples] = static_cast<double>(*latency) / 1'000'000.0;
        ++state.count;
        constexpr u64 LogIntervalNs = 10'000'000'000;
        if (now - state.last_log_ns < LogIntervalNs) {
            return;
        }
        state.last_log_ns = now;
        samples = state.samples_ms;
        count = state.count;
    }
    const Stats stats = Summarize(samples, count);
    LOG_INFO(Input, "Input to present: {} button changes, mean {:.2f} ms, p50 {:.2f}, p99 {:.2f}, "
                    "max {:.2f}",
             stats.samples, stats.mean_ms, stats.p50_ms, stats.p99_ms, stats.max_ms);
}

Stats Recent() {
    State& state = GetState();
    std::array<double, MaxSamples> samples;
    u64 count;
    {
        std::scoped_lock lock{state.mutex};
        samples = state.samples_ms;
        count = state.count;
    }
    return Summarize(samples, count);
}

} // namespace Common::InputLatency
