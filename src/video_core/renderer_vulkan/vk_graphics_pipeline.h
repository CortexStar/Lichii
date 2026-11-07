// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/nx2_bindcache.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_build.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key, std::array<vk::ShaderModule, NUM_STAGES> stages,
        const std::array<const Shader::Info*, NUM_STAGES>& infos,
        std::function<std::array<vk::ShaderModule, NUM_STAGES>()> emit_stages = {});

    static std::chrono::nanoseconds TakeHoldTime() noexcept;

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    const std::array<u64, 6>& ShaderHashes() const noexcept {
        return key.unique_hashes;
    }

    struct SampledImage {
        u32 stage;
        u32 tic;
        VideoCommon::ImageViewId id;
        VideoCommon::SamplerId sampler;
    };

    /// The sampled textures of the last configured draw, kept while a sampled frame dump runs.
    std::span<const SampledImage> SampledImages() const noexcept {
        return sampled_images;
    }

    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }

    /// Switch 2 task/mesh pipeline: drawn with vkCmdDrawMeshTasksEXT.
    [[nodiscard]] bool IsMeshPipeline() const noexcept {
        return stage_infos[0].is_task || stage_infos[0].is_mesh;
    }
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    /// A cheap digest of a key: keys with different tags differ.
    [[nodiscard]] static u64 TransitionTag(const GraphicsPipelineCacheKey& k) noexcept {
        u64 tag = 0;
        for (const u64 hash : k.unique_hashes) {
            tag = (tag ^ hash) * 0x9E3779B97F4A7C15ULL;
        }
        u64 formats{};
        std::memcpy(&formats, k.state.color_formats.data(),
                    (std::min)(sizeof(formats), sizeof(k.state.color_formats)));
        tag ^= (u64{k.state.raw1} << 32 | k.state.raw2) ^ (formats * 0xC2B2AE3D27D4EB4FULL);
        if (VideoCommon::Nx2BindMemo::PipeNextPointersEnabled()) {
            // Only bytes operator== compares: equal keys must tag alike.
            u64 more = k.state.attribute_types;
            if (offsetof(FixedPipelineState, attachments) + sizeof(k.state.attachments) <=
                k.state.Size()) {
                for (const auto& attachment : k.state.attachments) {
                    more = (more ^ attachment.raw) * 0x9E3779B97F4A7C15ULL;
                }
            }
            tag ^= more * 0xD6E8FEB86659FD93ULL;
        }
        return tag;
    }

    bool Configure(bool is_indexed) {
        return configure_func(this, is_indexed);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        if (!VideoCommon::PipelineTransitionTagsEnabled()) {
            const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
            return it != transition_keys.end()
                       ? transitions[std::distance(transition_keys.begin(), it)]
                       : nullptr;
        }
        // EDEN_NX2_PIPE_TRANSITIONS: full keys are compared only where the tag matches, and a
        // transition taken moves one place toward the front.
        const u64 tag = TransitionTag(current_key);
        if (VideoCommon::Nx2BindMemo::PipeNextPointersEnabled()) {
            // A transition's key is its pipeline's own (AddTransition).
            for (size_t index = 0; index < transition_tags.size(); ++index) {
                GraphicsPipeline* const next = transitions[index];
                if (transition_tags[index] != tag || next->key != current_key) {
                    continue;
                }
                if (index != 0) {
                    std::swap(transition_tags[index], transition_tags[index - 1]);
                    std::swap(transitions[index], transitions[index - 1]);
                }
                return next;
            }
            return nullptr;
        }
        for (size_t index = 0; index < transition_tags.size(); ++index) {
            if (transition_tags[index] != tag || transition_keys[index] != current_key) {
                continue;
            }
            GraphicsPipeline* const next = transitions[index];
            if (index != 0) {
                std::swap(transition_tags[index], transition_tags[index - 1]);
                std::swap(transition_keys[index], transition_keys[index - 1]);
                std::swap(transitions[index], transitions[index - 1]);
            }
            return next;
        }
        return nullptr;
    }

    /// The frame the pipeline cache last handed this pipeline out (evicts long unused ones).
    void MarkUsed(u64 frame) noexcept {
        last_use_frame = frame;
    }

    [[nodiscard]] u64 LastUse() const noexcept {
        return last_use_frame;
    }

    /// Forgets the transitions, which may point to pipelines the cache evicted.
    void ClearTransitions() noexcept {
        transition_keys.clear();
        transitions.clear();
        transition_tags.clear();
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return build->IsBuilt();
    }

    [[nodiscard]] bool Failed() const noexcept {
        return build->Failed();
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { return pl->ConfigureImpl<Spec>(is_indexed); };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    void MakePipeline(VkRenderPass render_pass);

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;
    std::vector<u64> transition_tags; ///< TransitionTag of each of transition_keys
    u64 last_use_frame{};

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    std::vector<SampledImage> sampled_images;
    u32 num_textures{};
    bool fragment_has_color0_output{};
    bool writes_storage{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};
    std::vector<u32> binding_kinds; ///< EDEN_NX2_BINDMEMO: per texture/image view, in read order

    const bool worker_only_build;
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
    std::optional<PipelineBuild> build;
};

} // namespace Vulkan
