// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/types.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

using Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS;

[[nodiscard]] inline std::optional<PixelFormat> PixelFormatFromImageFormat(
    Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::Typeless:
        return std::nullopt;
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    }
    return std::nullopt;
}

[[nodiscard]] inline VkDeviceSize DescriptorSizeForType(const Device& device,
                                                        VkDescriptorType type) {
    const auto& props = device.DescriptorBufferProperties();
    const bool robust = device.IsRobustBufferAccessEnabled();
    switch (type) {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return robust ? props.robustUniformBufferDescriptorSize : props.uniformBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        return robust ? props.robustStorageBufferDescriptorSize : props.storageBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        return robust ? props.robustUniformTexelBufferDescriptorSize
                      : props.uniformTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        return robust ? props.robustStorageTexelBufferDescriptorSize
                      : props.storageTexelBufferDescriptorSize;
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        return props.combinedImageSamplerDescriptorSize;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        return props.storageImageDescriptorSize;
    default:
        return 0;
    }
}

struct DescriptorBufferBinding {
    VkDescriptorType type;
    u32 count;
    VkDeviceSize offset;
    VkDeviceSize stride;
};

struct DescriptorBufferLayout {
    VkDeviceSize size{};
    boost::container::small_vector<DescriptorBufferBinding, 32> bindings;

    [[nodiscard]] bool Empty() const noexcept {
        return bindings.empty();
    }
};

inline void WriteDescriptorBuffer(const Device& device, const DescriptorBufferLayout& layout,
                                  const DescriptorUpdateEntry* payload, u8* host) {
    const vk::Device& dev = device.GetLogical();
    for (const DescriptorBufferBinding& binding : layout.bindings) {
        for (u32 index = 0; index < binding.count; ++index) {
            const DescriptorUpdateEntry& entry = *(payload++);
            const VkDescriptorAddressInfoEXT address_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
                .pNext = nullptr,
                .address = entry.address.address,
                .range = entry.address.range,
                .format = entry.address.format,
            };
            VkDescriptorGetInfoEXT get_info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                .pNext = nullptr,
                .type = binding.type,
                .data{},
            };
            switch (binding.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                get_info.data.pUniformBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                get_info.data.pStorageBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                get_info.data.pUniformTexelBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                get_info.data.pStorageTexelBuffer = &address_info;
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                get_info.data.pCombinedImageSampler = &entry.image;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                get_info.data.pStorageImage = &entry.image;
                break;
            default:
                continue;
            }
            dev.GetDescriptorEXT(get_info, binding.stride,
                                 host + binding.offset + index * binding.stride);
        }
    }
}

[[nodiscard]] inline u32 NumDescriptorEntries(const Shader::Info& info) {
    return Shader::NumDescriptors(info.constant_buffer_descriptors) +
           Shader::NumDescriptors(info.storage_buffers_descriptors) +
           Shader::NumDescriptors(info.texture_buffer_descriptors) +
           Shader::NumDescriptors(info.image_buffer_descriptors) +
           Shader::NumDescriptors(info.texture_descriptors) +
           Shader::NumDescriptors(info.image_descriptors);
}

class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder(const Device& device_) : device{&device_} {}

    bool CanUsePushDescriptor() const noexcept {
        if (!device->IsKhrPushDescriptorSupported() ||
            num_descriptors > device->MaxPushDescriptors()) {
            return false;
        }
        return !device->IsExtDescriptorBufferSupported() ||
               device->DescriptorBufferProperties().bufferlessPushDescriptors;
    }

    bool CanUseDescriptorBuffer() const noexcept {
        const auto& props = device->DescriptorBufferProperties();
        if (!device->IsExtDescriptorBufferSupported() || bindings.empty() ||
            !props.combinedImageSamplerDescriptorSingleArray) {
            return false;
        }
        return !props.bufferlessPushDescriptors || !CanUsePushDescriptor();
    }

    DescriptorBufferLayout MakeDescriptorBufferLayout(VkDescriptorSetLayout layout) const {
        DescriptorBufferLayout result;
        if (!layout) {
            return result;
        }
        const vk::Device& dev = device->GetLogical();
        result.size = dev.GetDescriptorSetLayoutSizeEXT(layout);
        result.bindings.reserve(bindings.size());
        for (const VkDescriptorSetLayoutBinding& entry : bindings) {
            result.bindings.push_back(DescriptorBufferBinding{
                .type = entry.descriptorType,
                .count = entry.descriptorCount,
                .offset = dev.GetDescriptorSetLayoutBindingOffsetEXT(layout, entry.binding),
                .stride = DescriptorSizeForType(*device, entry.descriptorType),
            });
        }
        return result;
    }

    vk::DescriptorSetLayout CreateDescriptorSetLayout(bool use_push_descriptor,
                                                      bool use_descriptor_buffer = false) const {
        if (bindings.empty()) {
            return nullptr;
        }
        VkDescriptorSetLayoutCreateFlags flags = 0;
        if (use_push_descriptor) {
            flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        }
        if (use_descriptor_buffer) {
            flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
        }
        boost::container::small_vector<VkDescriptorBindingFlags, 32> binding_flags;
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_ci{};
        const void* pnext = nullptr;
        if (!use_push_descriptor && device->IsDescriptorBindingPartiallyBoundSupported()) {
            binding_flags.assign(bindings.size(), VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
            binding_flags_ci = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                .pNext = nullptr,
                .bindingCount = static_cast<u32>(binding_flags.size()),
                .pBindingFlags = binding_flags.data(),
            };
            pnext = &binding_flags_ci;
        }
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = pnext,
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
    }

    vk::DescriptorUpdateTemplate CreateTemplate(VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout pipeline_layout,
                                                bool use_push_descriptor) const {
        if (entries.empty()) {
            return nullptr;
        }
        const VkDescriptorUpdateTemplateType type =
            use_push_descriptor ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
                                : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        return device->GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
            .pDescriptorUpdateEntries = entries.data(),
            .templateType = type,
            .descriptorSetLayout = descriptor_set_layout,
            .pipelineBindPoint =
                is_compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = pipeline_layout,
            .set = 0,
        });
    }

    vk::PipelineLayout CreatePipelineLayout(VkDescriptorSetLayout descriptor_set_layout) const {
        using Shader::Backend::SPIRV::RenderAreaLayout;
        using Shader::Backend::SPIRV::RescalingLayout;
        const u32 size_offset = is_compute ? sizeof(RescalingLayout::down_factor) : 0u;
        const VkPushConstantRange range{
            .stageFlags = static_cast<VkShaderStageFlags>(
                is_compute ? VK_SHADER_STAGE_COMPUTE_BIT
                           : (uses_mesh ? VK_SHADER_STAGE_ALL_GRAPHICS |
                                              VK_SHADER_STAGE_TASK_BIT_EXT |
                                              VK_SHADER_STAGE_MESH_BIT_EXT
                                        : VK_SHADER_STAGE_ALL_GRAPHICS)),
            .offset = 0,
            .size = static_cast<u32>(sizeof(RescalingLayout)) - size_offset +
                    static_cast<u32>(sizeof(RenderAreaLayout)),
        };
        return device->GetLogical().CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = descriptor_set_layout ? 1U : 0U,
            .pSetLayouts = bindings.empty() ? nullptr : &descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
    }

    void Add(const Shader::Info& info, VkShaderStageFlags stage) {
        is_compute |= (stage & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
        uses_mesh |= (stage & (VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) != 0;

        Add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stage, info.constant_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, info.storage_buffers_descriptors);
        Add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, stage, info.texture_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, stage, info.image_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, info.texture_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stage, info.image_descriptors);
    }

private:
    template <typename Descriptors>
    void Add(VkDescriptorType type, VkShaderStageFlags stage, const Descriptors& descriptors) {
        const size_t num{descriptors.size()};
        for (size_t i = 0; i < num; ++i) {
            bindings.push_back({
                .binding = binding,
                .descriptorType = type,
                .descriptorCount = descriptors[i].count,
                .stageFlags = stage,
                .pImmutableSamplers = nullptr,
            });
            entries.push_back({
                .dstBinding = binding,
                .dstArrayElement = 0,
                .descriptorCount = descriptors[i].count,
                .descriptorType = type,
                .offset = offset,
                .stride = sizeof(DescriptorUpdateEntry),
            });
            ++binding;
            num_descriptors += descriptors[i].count;
            offset += sizeof(DescriptorUpdateEntry) * descriptors[i].count;
        }
    }

    const Device* device{};
    bool is_compute{};
    bool uses_mesh{};
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 32> bindings;
    boost::container::small_vector<VkDescriptorUpdateTemplateEntry, 32> entries;
    u32 binding{};
    u32 num_descriptors{};
    size_t offset{};
};

class RescalingPushConstant {
public:
    explicit RescalingPushConstant() noexcept {}

    void PushTexture(bool is_rescaled) noexcept {
        *texture_ptr |= is_rescaled ? texture_bit : 0u;
        texture_bit <<= 1u;
        if (texture_bit == 0u) {
            texture_bit = 1u;
            ++texture_ptr;
        }
    }

    void PushImage(bool is_rescaled) noexcept {
        *image_ptr |= is_rescaled ? image_bit : 0u;
        image_bit <<= 1u;
        if (image_bit == 0u) {
            image_bit = 1u;
            ++image_ptr;
        }
    }

    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& Data() const noexcept {
        return words;
    }

private:
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> words{};
    u32* texture_ptr{words.data()};
    u32* image_ptr{words.data() + Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

class RenderAreaPushConstant {
public:
    bool uses_render_area{};
    std::array<f32, 4> words{};
};

/// The texture and image bindings of a pipeline's latest draws or dispatches. One with the same
/// handles reuses the resolved view and sampler ids and image descriptors while the descriptor
/// pool entries behind the handles and the texture cache's BindingsEpoch() stay the same
/// (EDEN_NX2_BINDCACHE).
class ImageBindingCache {
public:
    struct Entry {
        u64 epoch{}; ///< TextureCache::BindingsEpoch() of the configuration; 0: unusable
        bool via_header_index{};
        std::vector<VideoCommon::ImageViewInOut> views;
        std::vector<u32> sampler_handles;
        std::vector<VideoCommon::SamplerId> samplers;
        std::vector<DescriptorUpdateEntry> descriptors; ///< what PushImageDescriptors added
        std::vector<u32> push_ends; ///< end of each PushImageDescriptors call in descriptors
        std::vector<u8> texture_rescaled; ///< PushTexture argument of each texture descriptor
        std::vector<u8> image_rescaled;   ///< PushImage argument of each image descriptor
        std::vector<VideoCommon::ImageId> written_images;
    };

    /// The entry for these handles when resolving them again would give the same bindings.
    [[nodiscard]] const Entry* Find(const TextureCache& texture_cache, bool compute,
                                    bool via_header_index,
                                    std::span<const VideoCommon::ImageViewInOut> views,
                                    std::span<const u32> sampler_handles) const {
        const u64 epoch = texture_cache.BindingsEpoch();
        for (const Entry& entry : entries) {
            if (entry.epoch != epoch || entry.via_header_index != via_header_index ||
                entry.views.size() != views.size() ||
                entry.sampler_handles.size() != sampler_handles.size() ||
                !std::equal(sampler_handles.begin(), sampler_handles.end(),
                            entry.sampler_handles.begin())) {
                continue;
            }
            bool same_handles = true;
            for (size_t index = 0; index < views.size(); ++index) {
                if (entry.views[index].index != views[index].index) {
                    same_handles = false;
                    break;
                }
            }
            if (!same_handles) {
                continue;
            }
            for (const VideoCommon::ImageViewInOut& view : entry.views) {
                if (!texture_cache.ImageBindingUnchanged(view.index, compute, view.id)) {
                    return nullptr;
                }
            }
            for (size_t index = 0; index < entry.samplers.size(); ++index) {
                if (!texture_cache.SamplerBindingUnchanged(entry.sampler_handles[index], compute,
                                                           entry.samplers[index])) {
                    return nullptr;
                }
            }
            return &entry;
        }
        return nullptr;
    }

    /// Starts recording a configuration resolved from scratch. Usable once its epoch is set.
    [[nodiscard]] Entry& BeginRecord(bool via_header_index,
                                     std::span<const VideoCommon::ImageViewInOut> views,
                                     std::span<const u32> sampler_handles,
                                     std::span<const VideoCommon::SamplerId> samplers) {
        Entry& entry = entries[next];
        next = (next + 1) % static_cast<u32>(entries.size());
        entry.epoch = 0;
        entry.via_header_index = via_header_index;
        entry.views.assign(views.begin(), views.end());
        entry.sampler_handles.assign(sampler_handles.begin(), sampler_handles.end());
        entry.samplers.assign(samplers.begin(), samplers.end());
        entry.descriptors.clear();
        entry.push_ends.clear();
        entry.texture_rescaled.clear();
        entry.image_rescaled.clear();
        entry.written_images.clear();
        return entry;
    }

private:
    std::array<Entry, 4> entries;
    u32 next{};
};

/// Adds the descriptors that call `push_index` of PushImageDescriptors added for the entry.
inline void ReplayImageDescriptors(const ImageBindingCache::Entry& entry, size_t push_index,
                                   GuestDescriptorQueue& guest_descriptor_queue) {
    const u32 begin = push_index == 0 ? 0 : entry.push_ends[push_index - 1];
    const u32 end = entry.push_ends[push_index];
    guest_descriptor_queue.AddEntries(entry.descriptors.data() + begin, end - begin);
}

/// What PushImageDescriptors did besides adding descriptors: rescaling bits, written images.
inline void ReplayImageBindingEffects(const ImageBindingCache::Entry& entry,
                                      TextureCache& texture_cache,
                                      RescalingPushConstant& rescaling) {
    for (const u8 is_rescaled : entry.texture_rescaled) {
        rescaling.PushTexture(is_rescaled != 0);
    }
    for (const u8 is_rescaled : entry.image_rescaled) {
        rescaling.PushImage(is_rescaled != 0);
    }
    for (const VideoCommon::ImageId image_id : entry.written_images) {
        texture_cache.MarkModification(image_id);
    }
}

inline void PushImageDescriptors(TextureCache& texture_cache,
                                 GuestDescriptorQueue& guest_descriptor_queue,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VideoCommon::SamplerId*& samplers,
                                 const VideoCommon::ImageViewInOut*& views,
                                 ImageBindingCache::Entry* record = nullptr) {
    const size_t first_descriptor = guest_descriptor_queue.UpdateCount();
    const u32 num_texture_buffers = Shader::NumDescriptors(info.texture_buffer_descriptors);
    const u32 num_image_buffers = Shader::NumDescriptors(info.image_buffer_descriptors);
    views += num_texture_buffers;
    views += num_image_buffers;
    for (const auto& desc : info.texture_descriptors) {
        bool is_rescaled{};
        for (u32 index = 0; index < desc.count; ++index) {
            const VideoCommon::ImageViewId image_view_id{(views++)->id};
            const VideoCommon::SamplerId sampler_id{*(samplers++)};
            ImageView& image_view{texture_cache.GetImageView(image_view_id)};
            VkImageView vk_image_view{image_view.Handle(desc.type)};
            if (vk_image_view == VK_NULL_HANDLE) {
                const VkImageView null_image_view{texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID).Handle(desc.type)};
                if (null_image_view != VK_NULL_HANDLE) vk_image_view = null_image_view;
            }
            Sampler& sampler{texture_cache.GetSampler(sampler_id)};
            guest_descriptor_queue.AddSampledImage(vk_image_view,
                                                   sampler.HandleFor(image_view, desc.is_depth));
            const bool element_rescaled{texture_cache.IsRescaling(image_view)};
            is_rescaled |= element_rescaled;
        }
        rescaling.PushTexture(is_rescaled);
        if (record) {
            record->texture_rescaled.push_back(is_rescaled ? 1 : 0);
        }
    }
    for (const auto& desc : info.image_descriptors) {
        bool is_rescaled{};
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            const VkImageView vk_image_view{
                image_view.StorageView(desc.type, desc.format, desc.is_byte_addressed)};
            if (desc.is_written) {
                texture_cache.MarkModification(image_view.image_id);
                if (record) {
                    record->written_images.push_back(image_view.image_id);
                }
            }
            guest_descriptor_queue.AddImage(vk_image_view);
            const bool element_rescaled{texture_cache.IsRescaling(image_view)};
            is_rescaled |= element_rescaled;
        }
        rescaling.PushImage(is_rescaled);
        if (record) {
            record->image_rescaled.push_back(is_rescaled ? 1 : 0);
        }
    }
    if (record) {
        const DescriptorUpdateEntry* const data = guest_descriptor_queue.UpdateData();
        record->descriptors.insert(record->descriptors.end(), data + first_descriptor,
                                   data + guest_descriptor_queue.UpdateCount());
        record->push_ends.push_back(static_cast<u32>(record->descriptors.size()));
    }
}

} // namespace Vulkan
