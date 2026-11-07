// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <map>
#include <span>

#include "common/common_types.h"

namespace Vulkan {

template <typename ReadConstant>
bool MatchesStaticCompute(std::span<const u64> expected_code, std::span<const u64> actual_code,
                          const std::map<u64, u32>& constants, ReadConstant&& read) {
    if (expected_code.size() != actual_code.size() ||
        !std::equal(expected_code.begin(), expected_code.end(), actual_code.begin())) return false;
    for (const auto& [key, expected] : constants) {
        const auto actual = read(static_cast<u32>(key >> 32), static_cast<u32>(key));
        if (!actual || *actual != expected) return false;
    }
    return true;
}

} // namespace Vulkan
