// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

// VK_EXT_shader_object (notes/native-gpu-backend.md step 6): a graphics pipeline's stages as
// linked shader objects instead of a pipeline. Every fixed-function state is dynamic then, so
// pipelines whose keys differ only in that state share one set of shaders.
namespace Vulkan::ShaderObject {

/// The graphics stages vkCmdBindShadersEXT binds every draw (absent ones bound to null).
inline constexpr std::array<VkShaderStageFlagBits, 7> BIND_STAGES{
    VK_SHADER_STAGE_VERTEX_BIT,
    VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
    VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
    VK_SHADER_STAGE_GEOMETRY_BIT,
    VK_SHADER_STAGE_FRAGMENT_BIT,
    VK_SHADER_STAGE_TASK_BIT_EXT,
    VK_SHADER_STAGE_MESH_BIT_EXT,
};

/// Index of a stage in BIND_STAGES.
[[nodiscard]] constexpr size_t BindIndex(VkShaderStageFlagBits stage) noexcept {
    for (size_t index = 0; index < BIND_STAGES.size(); ++index) {
        if (BIND_STAGES[index] == stage) {
            return index;
        }
    }
    return BIND_STAGES.size();
}

/// The stages one bind sets (every stage the device has must have a shader or null bound):
/// without mesh shaders the task and mesh stages are left out.
[[nodiscard]] constexpr u32 NumBindStages(bool mesh_shaders) noexcept {
    return mesh_shaders ? 7U : 5U;
}

/// One pipeline's linked shaders, and the handle to bind per BIND_STAGES entry (null for the
/// stages it doesn't have).
struct Set {
    std::vector<vk::ShaderEXT> shaders;
    std::array<VkShaderEXT, BIND_STAGES.size()> bind{};
    u32 num_bind{};
};

/// What makes two pipelines' shaders the same: each stage's SPIR-V, the stage it runs as, and
/// the resource interface (set layout signature, heap mode).
struct Key {
    std::array<u128, 5> spirv_hashes{};
    std::array<u32, 5> stage_flags{};
    u64 interface_hash{};

    bool operator==(const Key&) const noexcept = default;
};

struct KeyHash {
    size_t operator()(const Key& key) const noexcept;
};

/// The stage each create info runs as, in order, with the stage after it (0: none). Linked
/// stages name the next linked stage; the last one names none.
struct StageChain {
    std::array<VkShaderStageFlagBits, 5> stages{};
    std::array<VkShaderStageFlags, 5> next{};
    u32 count{};
};

/// Chains the present stages (`stages[i]` 0 when absent) in order.
[[nodiscard]] StageChain ChainStages(const std::array<VkShaderStageFlagBits, 5>& stages) noexcept;

/// Shader sets shared between the pipelines that have the same Key. A set lives as long as a
/// pipeline holds it (pipeline eviction frees the shaders as it freed pipelines).
class Cache {
public:
    using BuildFunc = std::function<std::shared_ptr<const Set>()>;

    /// The set for the key, built with `build` if no pipeline made it yet. Any thread.
    std::shared_ptr<const Set> Get(const Key& key, const BuildFunc& build);

    /// Sets some pipeline still holds.
    [[nodiscard]] size_t Size() const;

private:
    mutable std::mutex mutex;
    std::unordered_map<Key, std::weak_ptr<const Set>, KeyHash> sets;
};

} // namespace Vulkan::ShaderObject
