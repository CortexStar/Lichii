// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace Vulkan {

constexpr bool UseApplicationPipelineCache(bool enabled, bool switch2, bool vendor_cache) {
    return enabled && (switch2 || !vendor_cache);
}

constexpr bool FlushPipelineCacheDuringGameplay(bool switch2, bool vendor_cache) {
    return !switch2 || !vendor_cache;
}

constexpr bool NeedsNupBlendDepthVariant(bool dynamic_depth, bool dynamic_blend) {
    return !dynamic_depth || !dynamic_blend;
}

enum class CacheReadResult { Complete, Retry, Failed };

template <typename Read>
std::optional<std::vector<char>> ReadPipelineCacheSnapshot(Read&& read) {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        size_t size = 0;
        const auto query = read(&size, nullptr);
        if (query == CacheReadResult::Failed) return std::nullopt;
        if (query == CacheReadResult::Retry) continue;
        std::vector<char> data(size);
        const auto result = read(&size, data.data());
        if (result == CacheReadResult::Failed) return std::nullopt;
        if (result == CacheReadResult::Complete) {
            if (size > data.size()) return std::nullopt;
            data.resize(size);
            return data;
        }
    }
    return std::nullopt;
}

} // namespace Vulkan
