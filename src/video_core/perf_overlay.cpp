// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "common/settings.h"
#include "video_core/perf_overlay.h"

namespace VideoCore::PerfOverlay {

bool Enabled() {
    static const bool forced = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_OVERLAY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return forced || Settings::values.perf_overlay.GetValue();
}

std::array<float, 3> EncodeColor(const std::array<float, 3>& linear, float white_nits,
                                 Encoding encoding) {
    std::array<float, 3> out{};
    switch (encoding) {
    case Encoding::Srgb:
        for (size_t i = 0; i < 3; ++i) {
            const float c = std::clamp(linear[i], 0.0f, 1.0f);
            out[i] = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        }
        break;
    case Encoding::ScRgb:
        for (size_t i = 0; i < 3; ++i) {
            out[i] = linear[i] * white_nits / 80.0f;
        }
        break;
    case Encoding::Pq: {
        // BT.709 to BT.2020 primaries (ITU-R BT.2087), then ST 2084 of nits / 10000.
        static constexpr std::array<std::array<float, 3>, 3> To2020{{
            {0.627404f, 0.329283f, 0.043313f},
            {0.069097f, 0.919540f, 0.011362f},
            {0.016391f, 0.088013f, 0.895595f},
        }};
        constexpr float m1 = 2610.0f / 16384.0f;
        constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
        constexpr float c1 = 3424.0f / 4096.0f;
        constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
        constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
        for (size_t i = 0; i < 3; ++i) {
            const float c =
                To2020[i][0] * linear[0] + To2020[i][1] * linear[1] + To2020[i][2] * linear[2];
            const float y = std::clamp(c * white_nits / 10000.0f, 0.0f, 1.0f);
            const float p = std::pow(y, m1);
            out[i] = std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
        }
        break;
    }
    }
    return out;
}

float Quantile(std::span<float> values, float p) {
    if (values.empty()) {
        return 0.0f;
    }
    const float rank = static_cast<float>(values.size() - 1) * std::clamp(p, 0.0f, 1.0f);
    const auto lower = static_cast<size_t>(rank);
    std::nth_element(values.begin(), values.begin() + lower, values.end());
    const float low = values[lower];
    if (lower + 1 >= values.size()) {
        return low;
    }
    const float high = *std::min_element(values.begin() + lower + 1, values.end());
    return low + (high - low) * (rank - static_cast<float>(lower));
}

void Stats::OnFrame(Clock::time_point now) {
    if (last_frame) {
        const float ms = std::chrono::duration<float, std::milli>(now - *last_frame).count();
        frame_ms[frame_count % History] = ms;
        ++frame_count;
    }
    last_frame = now;
}

void Stats::SetRow(size_t row, std::span<const char> line) {
    const auto dst = std::span{text}.subspan(row * Columns, Columns);
    const size_t length = std::min(line.size(), Columns);
    std::copy_n(line.begin(), length, dst.begin());
    std::fill(dst.begin() + length, dst.end(), ' ');
}

void Stats::Refresh(Clock::time_point now, const Counters& counters) {
    next_refresh = now + RefreshInterval;
    const Sample current{
        .time = now,
        .frames = frame_count,
        .generated_frames = counters.generated_frames,
        .threads = counters.threads,
    };
    // Rates over the frames since the refresh before last: about a second, fine enough for the
    // 15.6 ms steps Windows counts thread CPU time in.
    const std::optional<Sample> base = samples[0] ? samples[0] : samples[1];
    samples[0] = samples[1];
    samples[1] = current;

    // Frame times of the last LowWindow, newest first.
    const size_t kept = std::min(frame_count, History);
    const float window_ms = std::chrono::duration<float, std::milli>(LowWindow).count();
    size_t count = 0;
    float sum_ms = 0.0f;
    while (count < kept && sum_ms < window_ms) {
        const float ms = frame_ms[(frame_count - 1 - count) % History];
        scratch[count++] = ms;
        sum_ms += ms;
    }
    const float average_ms = count != 0 ? sum_ms / static_cast<float>(count) : 0.0f;
    const float low_ms = Quantile(std::span{scratch}.first(count), 0.99f);

    // The steady frame time sits mid-graph; frames of twice that reach the top.
    graph_max_ms = average_ms > 0.0f ? std::max(1.0f, std::ceil(2.0f * average_ms)) : 33.4f;
    hitch_ms = average_ms * 1.5f;

    double seconds = 0.0;
    double real_fps = 0.0;
    double shown_fps = 0.0;
    u64 frames = 0;
    if (base) {
        seconds = std::chrono::duration<double>(now - base->time).count();
        frames = current.frames - base->frames;
        if (seconds > 0.0) {
            real_fps = static_cast<double>(frames) / seconds;
            const u64 generated = current.generated_frames >= base->generated_frames
                                      ? current.generated_frames - base->generated_frames
                                      : 0;
            shown_fps = static_cast<double>(frames + generated) / seconds;
        }
    }
    PutRow(0, "FPS {:6.1f} real {:6.1f} shown", real_fps, shown_fps);
    if (count != 0) {
        PutRow(1, "Frame {:5.2f} ms  1% low {:5.1f} fps", average_ms, 1000.0f / low_ms);
    } else {
        PutRow(1, "Frame    -- ms  1% low    -- fps");
    }

    // CPU time per real frame of a thread, as text ("--" when not running).
    const auto per_frame = [&](std::optional<u64> ThreadTimes::* member, std::span<char, 6> out) {
        const auto& before = base ? (base->threads.*member) : std::optional<u64>{};
        const auto& after = current.threads.*member;
        if (frames == 0 || !before || !after || *after < *before) {
            fmt::format_to_n(out.data(), out.size(), "{:>6}", "--");
            return;
        }
        const double ms = static_cast<double>(*after - *before) / 1e6 / static_cast<double>(frames);
        fmt::format_to_n(out.data(), out.size(), "{:6.2f}", ms);
    };
    std::array<char, 6> front{};
    std::array<char, 6> back{};
    std::array<char, 6> worker{};
    per_frame(&ThreadTimes::front, front);
    per_frame(&ThreadTimes::back, back);
    per_frame(&ThreadTimes::worker, worker);
    const auto view = [](const std::array<char, 6>& field) {
        return std::string_view{field.data(), field.size()};
    };
    PutRow(2, "CPU front{} back{} ms", view(front), view(back));
    std::array<char, 6> gpu{};
    if (counters.gpu_frame_ms) {
        fmt::format_to_n(gpu.data(), gpu.size(), "{:6.2f}", *counters.gpu_frame_ms);
    } else {
        fmt::format_to_n(gpu.data(), gpu.size(), "{:>6}", "--");
    }
    PutRow(3, "CPU submit{} GPU{} ms", view(worker), view(gpu));
    PutRow(4, "Frame time graph, top {:.0f} ms", graph_max_ms);
}

void Stats::Graph(std::span<float, GraphSamples> out) const {
    const size_t kept = std::min(frame_count, std::min(History, GraphSamples));
    const size_t empty = GraphSamples - kept;
    std::fill_n(out.begin(), empty, 0.0f);
    for (size_t i = 0; i < kept; ++i) {
        const float ms = frame_ms[(frame_count - kept + i) % History];
        const float value = ms / graph_max_ms;
        out[empty + i] = hitch_ms > 0.0f && ms > hitch_ms ? -value : value;
    }
}

} // namespace VideoCore::PerfOverlay
