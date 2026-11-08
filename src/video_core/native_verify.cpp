// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdlib>
#include <mutex>

#include "video_core/native_verify.h"

namespace VideoCommon::NativeVerify {

u32 ParseMask(std::string_view value) {
    u32 mask = 0;
    while (!value.empty()) {
        const size_t comma = value.find(',');
        const std::string_view name = value.substr(0, comma);
        for (size_t layer = 0; layer < layer_names.size(); ++layer) {
            if (name == layer_names[layer] || name == "all") {
                mask |= 1U << layer;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return mask;
}

u32 EnabledMask() {
    const char* const value = std::getenv("EDEN_NX2_NATIVE_VERIFY");
    return value == nullptr ? 0 : ParseMask(value);
}

Counters& CountersOf(Layer layer) {
    static std::array<Counters, static_cast<size_t>(Layer::Count)> counters;
    return counters[static_cast<size_t>(layer)];
}

void Report() {
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static Clock::time_point last = Clock::now();
    const Clock::time_point now = Clock::now();
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (now - last < std::chrono::seconds{10}) {
        return;
    }
    last = now;
    for (size_t index = 0; index < layer_names.size(); ++index) {
        const auto layer = static_cast<Layer>(index);
        if (!Enabled(layer)) {
            continue;
        }
        Counters& counters = CountersOf(layer);
        LOG_INFO(HW_GPU, "Native verify {} (10 s): {} checks, {} differences", layer_names[index],
                 counters.checks.exchange(0, std::memory_order_relaxed),
                 counters.differences.exchange(0, std::memory_order_relaxed));
    }
}

} // namespace VideoCommon::NativeVerify
