// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <utility>

#include <fmt/format.h>

#include "common/common_types.h"

// The performance overlay the presenter draws over the game: what it shows and how its colours
// are encoded for the output. The Vulkan side (present/perf_overlay_pass) only draws the text and
// graph built here.
namespace VideoCore::PerfOverlay {

/// The perf_overlay setting, or EDEN_NX2_PERF_OVERLAY=1 (read once).
[[nodiscard]] bool Enabled();

/// How the presented image encodes colour.
enum class Encoding : u32 {
    Srgb,  ///< SDR, written through a UNORM view: sRGB-encoded values
    Pq,    ///< HDR10: BT.2020 primaries, SMPTE ST 2084
    ScRgb, ///< Linear BT.709, 1.0 = 80 nits
};

/// Encodes a linear BT.709 colour whose 1.0 is shown at white_nits (HDR outputs; SDR white is 1).
[[nodiscard]] std::array<float, 3> EncodeColor(const std::array<float, 3>& linear, float white_nits,
                                               Encoding encoding);

/// The p-th quantile (0..1) of values, interpolated between the closest ranks like oot_bench's
/// percentile (numpy's "linear"). Reorders values; 0 when empty.
[[nodiscard]] float Quantile(std::span<float> values, float p);

/// Cumulative CPU time of the threads the overlay shows, in nanoseconds (nullopt: not running or
/// unreadable).
struct ThreadTimes {
    std::optional<u64> front;  ///< "GPU": the GPU thread, the split's front half when it runs
    std::optional<u64> back;   ///< "GPUBack": the split's back half
    std::optional<u64> worker; ///< "VkWorker": the Vulkan scheduler's submit thread
};

/// Cumulative counters read at each refresh.
struct Counters {
    u64 generated_frames{}; ///< Frames frame generation presented since start
    ThreadTimes threads;
    std::optional<float> gpu_frame_ms; ///< GPU time of a recent frame, from timestamps
    float white_nits{};                ///< Text white on HDR outputs
};

class Stats {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr size_t Columns = 34;
    static constexpr size_t Rows = 5;
    static constexpr size_t GraphSamples = 136; ///< Frame times in the graph, newest last
    static constexpr size_t History = 4096;     ///< Frame times kept for the lows
    static constexpr Clock::duration RefreshInterval = std::chrono::milliseconds{500};
    static constexpr Clock::duration LowWindow = std::chrono::seconds{5};

    /// A real (guest) frame was presented at now.
    void OnFrame(Clock::time_point now);

    /// Whether Refresh is due at now.
    [[nodiscard]] bool RefreshDue(Clock::time_point now) const {
        return now >= next_refresh;
    }

    /// Rebuilds the text from the frames since the refresh before last (about a second) and the
    /// last LowWindow of frame times.
    void Refresh(Clock::time_point now, const Counters& counters);

    /// Rows x Columns characters, space padded.
    [[nodiscard]] std::span<const char> Text() const {
        return text;
    }

    /// The last GraphSamples frame times divided by GraphMaxMs (oldest first, 0 before the first
    /// frames); negative where a frame took longer than 1.5 times the average (a hitch).
    void Graph(std::span<float, GraphSamples> out) const;

    [[nodiscard]] float GraphMaxMs() const {
        return graph_max_ms;
    }

private:
    struct Sample {
        Clock::time_point time;
        u64 frames{};
        u64 generated_frames{};
        ThreadTimes threads;
    };

    void SetRow(size_t row, std::span<const char> line);

    template <typename... Args>
    void PutRow(size_t row, fmt::format_string<Args...> format, Args&&... args) {
        std::array<char, 64> line{};
        const auto result =
            fmt::format_to_n(line.data(), line.size(), format, std::forward<Args>(args)...);
        SetRow(row, std::span<const char>{line}.first(std::min(result.size, line.size())));
    }

    std::array<float, History> frame_ms{}; ///< Ring of frame times
    std::array<float, History> scratch{};
    size_t frame_count{}; ///< Frames recorded, total
    std::optional<Clock::time_point> last_frame;
    std::array<std::optional<Sample>, 2> samples{}; ///< The last two refreshes, oldest first
    Clock::time_point next_refresh{};
    float graph_max_ms{33.4f};
    float hitch_ms{};
    std::array<char, Rows * Columns> text{};
};

} // namespace VideoCore::PerfOverlay
