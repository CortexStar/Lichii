// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "common/input_latency.h"
#include "core/hle/service/nx2_present_pacer.h"
#include "video_core/guest_frame_timing.h"
#include "video_core/host_vsync.h"

namespace VideoCore::HostVsync {

namespace {

struct State {
    std::mutex mutex;
    std::function<void()> listener;
    std::function<void(u64, u32)> input_stage;
    bool stage_limits{};
    std::optional<Clock::time_point> last_start;
    std::atomic<u32> rate{};
};

State& GetState() {
    static State state;
    return state;
}

void WaitUntil(Clock::time_point deadline) {
    using namespace std::chrono_literals;
    while (true) {
        const auto left = deadline - Clock::now();
        if (left <= Clock::duration::zero()) {
            return;
        }
        if (left > 2ms) {
            std::this_thread::sleep_for(left - 1ms);
        } else {
            std::this_thread::yield();
        }
    }
}

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_HOST_VSYNC");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

void SetListener(std::function<void()> listener) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.listener = std::move(listener);
    state.last_start.reset();
}

void SetInputStage(std::function<void(u64, u32)> stage, bool limits) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.input_stage = std::move(stage);
    state.stage_limits = limits;
}

void SetRate(u32 rate) {
    GetState().rate.store(rate, std::memory_order_relaxed);
}

void Displayed() {
    if (!Enabled()) {
        return;
    }
    State& state = GetState();
    std::unique_lock lock{state.mutex};
    if (!state.listener) {
        return;
    }
    const u32 rate = state.rate.load(std::memory_order_relaxed);
    if (!state.stage_limits) {
        const auto deadline = Service::DisplayVsyncDeadline(state.last_start, Clock::now(), rate);
        lock.unlock();
        WaitUntil(deadline);
        lock.lock();
        if (!state.listener) {
            return;
        }
    }
    if (state.input_stage) {
        state.input_stage(GuestFrameTiming::Current().id + 1, state.stage_limits ? rate : 0);
    }
    static_cast<void>(GuestFrameTiming::BeginFrame());
    // The game starts its next frame here and samples input then (after Anti-Lag 2's input
    // stage, which may hold this thread).
    Common::InputLatency::FrameStart();
    state.last_start = Clock::now();
    state.listener();
}

std::optional<Clock::time_point> LastStart() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    return state.last_start;
}

} // namespace VideoCore::HostVsync
