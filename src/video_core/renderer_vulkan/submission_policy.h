// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <limits>
#include <optional>
#include <string_view>

#include "common/common_types.h"

namespace Vulkan::SubmissionPolicy {

inline constexpr u32 DefaultNativeBudget = 128;
/// AMD's proprietary driver pays much more per submission. Ocarina of Time on an RX 9070 XT:
/// every 256 draws instead of 128 gave +10-12 FPS where it was already fast, every 512 a further
/// 35 -> 50 FPS at one spot standing still.
inline constexpr u32 AmdNativeBudget = 512;

/// Draws per submission of a native title when EDEN_NX2_FLUSH_DRAWS doesn't set it.
constexpr u32 NativeBudget(bool amd_proprietary) {
    return amd_proprietary ? AmdNativeBudget : DefaultNativeBudget;
}

enum class Action {
    None,
    Dispatch,
    Flush,
};

inline std::optional<u32> ParseBudget(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    u32 budget{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), budget);
    if (error != std::errc{} || end != value.data() + value.size() || budget == 0) {
        return std::nullopt;
    }
    return budget;
}

/// Native desktop titles: a submission at the draw budget would end the render pass and drop
/// all recorded state (~400 render pass ends a second in Ocarina), so while `render_pass_active`
/// it waits for a draw that finds the pass ended, for at most four budgets.
constexpr Action Advance(u32& counter, bool native, bool android,
                         u32 native_budget = DefaultNativeBudget, u32 legacy_budget = 4096,
                         bool render_pass_active = false) {
    ++counter;
    if (native && !android) {
        if (counter >= native_budget &&
            (!render_pass_active || u64{counter} >= u64{native_budget} * 4 ||
             counter == (std::numeric_limits<u32>::max)())) {
            counter = 0;
            return Action::Flush;
        }
        return counter % 32 == 0 ? Action::Dispatch : Action::None;
    }
    const u32 check_mask = android ? 3 : 31;
    if ((counter & check_mask) != check_mask) {
        return Action::None;
    }
    if (counter < (android ? 512U : legacy_budget)) {
        return Action::Dispatch;
    }
    counter = 0;
    return Action::Flush;
}

} // namespace Vulkan::SubmissionPolicy
