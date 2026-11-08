// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_shader_object.h"

namespace Vulkan::ShaderObject {

size_t KeyHash::operator()(const Key& key) const noexcept {
    u64 hash = key.interface_hash ^ 0x9E3779B97F4A7C15ULL;
    const auto mix = [&hash](u64 value) { hash = (hash ^ value) * 0x100000001B3ULL; };
    for (size_t stage = 0; stage < key.spirv_hashes.size(); ++stage) {
        mix(key.spirv_hashes[stage][0]);
        mix(key.spirv_hashes[stage][1]);
        mix(key.stage_flags[stage]);
    }
    return static_cast<size_t>(hash ^ (hash >> 29));
}

StageChain ChainStages(const std::array<VkShaderStageFlagBits, 5>& stages) noexcept {
    StageChain chain;
    for (const VkShaderStageFlagBits stage : stages) {
        if (stage == 0) {
            continue;
        }
        if (chain.count != 0) {
            chain.next[chain.count - 1] = stage;
        }
        chain.stages[chain.count] = stage;
        chain.next[chain.count] = 0;
        ++chain.count;
    }
    return chain;
}

std::shared_ptr<const Set> Cache::Get(const Key& key, const BuildFunc& build) {
    {
        std::scoped_lock lock{mutex};
        if (const auto it = sets.find(key); it != sets.end()) {
            if (std::shared_ptr<const Set> set = it->second.lock()) {
                return set;
            }
        }
    }
    // Built outside the lock: shader builds of other pipelines go on meanwhile. Two pipelines
    // building the same set at once keep the first one inserted.
    std::shared_ptr<const Set> set = build();
    if (!set) {
        return nullptr;
    }
    std::scoped_lock lock{mutex};
    std::weak_ptr<const Set>& entry = sets[key];
    if (std::shared_ptr<const Set> other = entry.lock()) {
        return other;
    }
    entry = set;
    return set;
}

size_t Cache::Size() const {
    std::scoped_lock lock{mutex};
    size_t live = 0;
    for (const auto& [key, set] : sets) {
        live += set.expired() ? 0 : 1;
    }
    return live;
}

} // namespace Vulkan::ShaderObject
