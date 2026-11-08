// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

// Descriptor heap mode (VK_EXT_descriptor_heap) of a graphics pipeline: textures come from the
// heaps that mirror the guest pools (vk_texture_heap.h), each texture binding mapped at pipeline
// creation to a per-draw index table (HEAP_WITH_INDIRECT_INDEX_ARRAY). Uniform and storage buffers
// already go by device address (bank table, GPU page table), so a heap pipeline has no other
// set/binding resources. Each stage has its own table, at its own push data slot, holding one u32
// heap index per texture element of the stage: its image indices, then its sampler indices. A
// stage's mappings so depend on that stage alone, as the pipeline library cache shares a stage's
// library between pipelines.
namespace Vulkan::HeapMappings {

constexpr u32 IMAGE_HEAP_INDEX_STRIDE = 32;   ///< bytes per image descriptor
constexpr u32 SAMPLER_HEAP_INDEX_STRIDE = 16; ///< bytes per sampler descriptor
constexpr u32 NUM_STAGES = 5;

/// Push data offset of the stage tables' device addresses (u64 each): after the push block
/// shaders read (Shader::Backend::SPIRV::PushConstantLayout, 80 bytes).
constexpr u32 TABLE_PUSH_OFFSET = 80;
constexpr u32 PUSH_DATA_SIZE = TABLE_PUSH_OFFSET + NUM_STAGES * 8;

[[nodiscard]] constexpr u32 TablePushOffset(u32 stage) noexcept {
    return TABLE_PUSH_OFFSET + stage * 8;
}

/// One texture descriptor of a stage as the shader binds it.
struct TextureBinding {
    u32 stage;
    u32 set;
    u32 binding;
    u32 count;         ///< array elements
    u32 first_element; ///< its first element in the stage's table
};

struct TableLayout {
    std::vector<TextureBinding> bindings;
    std::array<u32, NUM_STAGES> stage_elements{}; ///< texture elements per stage

    [[nodiscard]] u32 ImageOffset(const TextureBinding& binding) const noexcept {
        return binding.first_element * 4;
    }
    [[nodiscard]] u32 SamplerOffset(const TextureBinding& binding) const noexcept {
        return (stage_elements[binding.stage] + binding.first_element) * 4;
    }
    [[nodiscard]] u32 TableBytes(u32 stage) const noexcept {
        return stage_elements[stage] * 2 * 4;
    }
};

/// Whether every resource of the stages other than combined image samplers goes by device
/// address, so the pipeline can run on the heaps with texture mappings only.
[[nodiscard]] inline bool Eligible(std::span<const Shader::Info> infos, bool mesh_emulation) {
    if (mesh_emulation) {
        return false;
    }
    for (const Shader::Info& info : infos) {
        if (!info.texture_buffer_descriptors.empty() || !info.image_buffer_descriptors.empty() ||
            !info.image_descriptors.empty() ||
            (!info.constant_buffer_descriptors.empty() && !info.uniforms_by_address) ||
            (!info.storage_buffers_descriptors.empty() && !info.storage_by_address)) {
            return false;
        }
    }
    return true;
}

/// The texture bindings in shader binding order. Monolithic pipelines put every stage in set 0
/// with bindings counted over the stages; pipeline libraries use one set per stage (set = stage
/// index) with bindings from 0 (as DescriptorLayoutBuilder and the SPIR-V emitter number them).
[[nodiscard]] inline TableLayout MakeTableLayout(std::span<const Shader::Info> infos,
                                                 bool set_per_stage) {
    TableLayout layout;
    u32 binding = 0;
    for (u32 stage = 0; stage < infos.size() && stage < NUM_STAGES; ++stage) {
        if (set_per_stage) {
            binding = 0;
        }
        for (const Shader::TextureDescriptor& desc : infos[stage].texture_descriptors) {
            layout.bindings.push_back({
                .stage = stage,
                .set = set_per_stage ? stage : 0U,
                .binding = binding++,
                .count = desc.count,
                .first_element = layout.stage_elements[stage],
            });
            layout.stage_elements[stage] += desc.count;
        }
    }
    return layout;
}

/// The mapping of one texture binding: heap indices read from its stage's table, whose device
/// address is at TablePushOffset(stage) in push data.
[[nodiscard]] inline VkDescriptorSetAndBindingMappingEXT MakeTextureMapping(
    const TableLayout& layout, const TextureBinding& binding) {
    const u32 table_push_offset = TablePushOffset(binding.stage);
    VkDescriptorSetAndBindingMappingEXT mapping{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,
        .pNext = nullptr,
        .descriptorSet = binding.set,
        .firstBinding = binding.binding,
        .bindingCount = 1,
        .resourceMask = VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT,
        .source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT,
        .sourceData = {},
    };
    mapping.sourceData.indirectIndexArray = VkDescriptorMappingSourceIndirectIndexArrayEXT{
        .heapOffset = 0,
        .pushOffset = table_push_offset,
        .addressOffset = layout.ImageOffset(binding),
        .heapIndexStride = IMAGE_HEAP_INDEX_STRIDE,
        .pEmbeddedSampler = nullptr,
        .useCombinedImageSamplerIndex = VK_FALSE,
        .samplerHeapOffset = 0,
        .samplerPushOffset = table_push_offset,
        .samplerAddressOffset = layout.SamplerOffset(binding),
        .samplerHeapIndexStride = SAMPLER_HEAP_INDEX_STRIDE,
    };
    return mapping;
}

/// Per stage, the mappings of its texture bindings.
[[nodiscard]] inline std::array<std::vector<VkDescriptorSetAndBindingMappingEXT>, NUM_STAGES>
MakeStageMappings(const TableLayout& layout) {
    std::array<std::vector<VkDescriptorSetAndBindingMappingEXT>, NUM_STAGES> stages;
    for (const TextureBinding& binding : layout.bindings) {
        stages[binding.stage].push_back(MakeTextureMapping(layout, binding));
    }
    return stages;
}

} // namespace Vulkan::HeapMappings
