// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_texture_heap.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {

namespace PoolMirror = VideoCommon::PoolMirror;

namespace {
// 1,048,576 image and 1,048,576 sampler slots. Copy-on-write keeps old page versions until the
// GPU is done with them, so the heaps hold several versions of the pools at once.
constexpr u32 IMAGE_HEAP_PAGES = 8192;   // 32 MiB
constexpr u32 SAMPLER_HEAP_PAGES = 4096; // 16 MiB
} // Anonymous namespace

TextureHeap::TextureHeap(const Device& device_, MemoryAllocator& memory_allocator)
    : device{device_} {
    // The heaps are buffers bound by device address: the allocator has to make them with
    // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    if (!device.IsExtDescriptorHeapSupported() || !device.HasNullDescriptor() ||
        !device.IsBufferDeviceAddressSupported()) {
        return;
    }
    const auto& properties = device.DescriptorHeapProperties();
    if (properties.imageDescriptorSize != PoolMirror::IMAGE_DESCRIPTOR_SIZE ||
        properties.samplerDescriptorSize != PoolMirror::SAMPLER_DESCRIPTOR_SIZE ||
        PoolMirror::PAGE_SIZE % properties.imageDescriptorAlignment != 0 ||
        PoolMirror::PAGE_SIZE % properties.samplerDescriptorAlignment != 0 ||
        properties.minResourceHeapReservedRange != 0 ||
        properties.minSamplerHeapReservedRange != 0) {
        LOG_INFO(Render_Vulkan,
                 "Descriptor heap: image/sampler descriptors are {}/{} B (the pool mirror lays out "
                 "{}/{} B) or the heaps need a reserved range; not used",
                 properties.imageDescriptorSize, properties.samplerDescriptorSize,
                 PoolMirror::IMAGE_DESCRIPTOR_SIZE, PoolMirror::SAMPLER_DESCRIPTOR_SIZE);
        return;
    }
    const auto make = [&](HeapBuffer& heap, u32 pages, VkDeviceSize max_size,
                          const char* name) -> bool {
        const VkDeviceSize size = VkDeviceSize{pages} * PoolMirror::PAGE_SIZE;
        if (size > max_size) {
            return false;
        }
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size,
            .usage =
                VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        heap.buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::Upload);
        if (!heap.buffer.IsHostVisible() || !heap.buffer.IsHostCoherent()) {
            LOG_INFO(Render_Vulkan, "Descriptor heap: {} heap memory is not host coherent", name);
            return false;
        }
        if (device.HasDebuggingToolAttached()) {
            heap.buffer.SetObjectNameEXT(name);
        }
        heap.host = heap.buffer.Mapped().data();
        heap.address = device.GetLogical().GetBufferDeviceAddress(*heap.buffer);
        heap.capacity_pages = pages;
        return true;
    };
    if (!make(images, IMAGE_HEAP_PAGES, properties.maxResourceHeapSize, "Texture heap") ||
        !make(samplers, SAMPLER_HEAP_PAGES, properties.maxSamplerHeapSize, "Sampler heap")) {
        return;
    }
    // The null fills: a null image descriptor (nullDescriptor) and a default sampler.
    const VkResourceDescriptorInfoEXT null_info{
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .data = {.pImage = nullptr},
    };
    const VkHostAddressRangeEXT null_image_range{null_image.data(), null_image.size()};
    device.GetLogical().WriteResourceDescriptorsEXT(null_info, &null_image_range);
    const VkSamplerCreateInfo default_sampler{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    const VkHostAddressRangeEXT null_sampler_range{null_sampler.data(), null_sampler.size()};
    device.GetLogical().WriteSamplerDescriptorsEXT(default_sampler, &null_sampler_range);
    usable = true;
    // The last page of each heap stays null: draws point elements without an entry there.
    usable = PreparePage(PoolMirror::Heap::Image, images.capacity_pages - 1,
                         PoolMirror::HeapPageVersions::NO_PAGE, true) &&
             PreparePage(PoolMirror::Heap::Sampler, samplers.capacity_pages - 1,
                         PoolMirror::HeapPageVersions::NO_PAGE, true);
    LOG_INFO(Render_Vulkan, "Descriptor heap: {} MiB texture heap, {} MiB sampler heap",
             (VkDeviceSize{IMAGE_HEAP_PAGES} * PoolMirror::PAGE_SIZE) >> 20,
             (VkDeviceSize{SAMPLER_HEAP_PAGES} * PoolMirror::PAGE_SIZE) >> 20);
}

TextureHeap::~TextureHeap() = default;

u8* TextureHeap::PageHost(const HeapBuffer& heap, u32 physical) const noexcept {
    return heap.host + size_t{physical} * PoolMirror::PAGE_SIZE;
}

bool TextureHeap::PreparePage(PoolMirror::Heap heap, u32 physical, u32 copy_from, bool reserved) {
    if (heap == PoolMirror::Heap::Meta) {
        // Meta words are read on the CPU only (index tables, rescaling bits).
        return true;
    }
    HeapBuffer& target = heap == PoolMirror::Heap::Image ? images : samplers;
    // The mirror's pages end before the reserved null page.
    const u32 limit = reserved ? target.capacity_pages : target.capacity_pages - 1;
    if (!usable || physical >= limit ||
        (copy_from != PoolMirror::HeapPageVersions::NO_PAGE && copy_from >= limit)) {
        if (usable) {
            LOG_ERROR(Render_Vulkan, "Descriptor heap: page {} past the {} pages of the {} heap",
                      physical, target.capacity_pages,
                      heap == PoolMirror::Heap::Image ? "texture" : "sampler");
        }
        usable = false;
        return false;
    }
    u8* const page = PageHost(target, physical);
    if (copy_from != PoolMirror::HeapPageVersions::NO_PAGE) {
        std::memcpy(page, PageHost(target, copy_from), PoolMirror::PAGE_SIZE);
        return true;
    }
    const std::span<const u8> fill = heap == PoolMirror::Heap::Image
                                         ? std::span<const u8>{null_image}
                                         : std::span<const u8>{null_sampler};
    for (size_t offset = 0; offset < PoolMirror::PAGE_SIZE; offset += fill.size()) {
        std::memcpy(page + offset, fill.data(), fill.size());
    }
    return true;
}

void TextureHeap::WriteImage(u32 physical, u32 logical_slot, const ImageView* view,
                             Shader::TextureType type) {
    if (!usable || physical >= images.capacity_pages) {
        return;
    }
    u8* const host =
        PageHost(images, physical) +
        size_t{logical_slot % PoolMirror::IMAGE_SLOTS_PER_PAGE} * PoolMirror::IMAGE_DESCRIPTOR_SIZE;
    ImageView::HeapViewInfo view_info{};
    if (view == nullptr || !view->HeapViewCreateInfo(type, view_info)) {
        std::memcpy(host, null_image.data(), null_image.size());
        return;
    }
    const VkImageDescriptorInfoEXT image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
        .pNext = nullptr,
        .pView = &view_info.create_info,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkResourceDescriptorInfoEXT info{
        .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
        .pNext = nullptr,
        .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        .data = {.pImage = &image_info},
    };
    const VkHostAddressRangeEXT range{host, PoolMirror::IMAGE_DESCRIPTOR_SIZE};
    device.GetLogical().WriteResourceDescriptorsEXT(info, &range);
}

void TextureHeap::WriteSampler(u32 physical, u32 logical_slot, const Sampler& sampler,
                               VkSampler variant) {
    if (!usable || physical >= samplers.capacity_pages) {
        return;
    }
    u8* const host =
        PageHost(samplers, physical) + size_t{logical_slot % PoolMirror::SAMPLER_SLOTS_PER_PAGE} *
                                           PoolMirror::SAMPLER_DESCRIPTOR_SIZE;
    Sampler::HeapSamplerInfo sampler_info{};
    if (!sampler.HeapSamplerCreateInfo(variant, sampler_info)) {
        std::memcpy(host, null_sampler.data(), null_sampler.size());
        return;
    }
    const VkHostAddressRangeEXT range{host, PoolMirror::SAMPLER_DESCRIPTOR_SIZE};
    device.GetLogical().WriteSamplerDescriptorsEXT(sampler_info.create_info, &range);
}

bool TextureHeap::HoldsImage(u32 index, const ImageView* view, Shader::TextureType type) const {
    if (!usable || index / PoolMirror::IMAGE_SLOTS_PER_PAGE >= images.capacity_pages) {
        return false;
    }
    std::array<u8, PoolMirror::IMAGE_DESCRIPTOR_SIZE> expected{null_image};
    ImageView::HeapViewInfo view_info{};
    if (view != nullptr && view->HeapViewCreateInfo(type, view_info)) {
        const VkImageDescriptorInfoEXT image_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .pView = &view_info.create_info,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkResourceDescriptorInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
            .pNext = nullptr,
            .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .data = {.pImage = &image_info},
        };
        const VkHostAddressRangeEXT range{expected.data(), expected.size()};
        device.GetLogical().WriteResourceDescriptorsEXT(info, &range);
    }
    return std::memcmp(images.host + size_t{index} * PoolMirror::IMAGE_DESCRIPTOR_SIZE,
                       expected.data(), expected.size()) == 0;
}

bool TextureHeap::HoldsSampler(u32 index, const Sampler& sampler, VkSampler variant) const {
    if (!usable || index / PoolMirror::SAMPLER_SLOTS_PER_PAGE >= samplers.capacity_pages) {
        return false;
    }
    std::array<u8, PoolMirror::SAMPLER_DESCRIPTOR_SIZE> expected{null_sampler};
    Sampler::HeapSamplerInfo sampler_info{};
    if (sampler.HeapSamplerCreateInfo(variant, sampler_info)) {
        const VkHostAddressRangeEXT range{expected.data(), expected.size()};
        device.GetLogical().WriteSamplerDescriptorsEXT(sampler_info.create_info, &range);
    }
    return std::memcmp(samplers.host + size_t{index} * PoolMirror::SAMPLER_DESCRIPTOR_SIZE,
                       expected.data(), expected.size()) == 0;
}

VkBindHeapInfoEXT TextureHeap::ResourceHeapBinding() const noexcept {
    return VkBindHeapInfoEXT{
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext = nullptr,
        .heapRange = {.address = images.address,
                      .size = VkDeviceSize{images.capacity_pages} * PoolMirror::PAGE_SIZE},
        .reservedRangeOffset = 0,
        .reservedRangeSize = 0,
    };
}

VkBindHeapInfoEXT TextureHeap::SamplerHeapBinding() const noexcept {
    return VkBindHeapInfoEXT{
        .sType = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .pNext = nullptr,
        .heapRange = {.address = samplers.address,
                      .size = VkDeviceSize{samplers.capacity_pages} * PoolMirror::PAGE_SIZE},
        .reservedRangeOffset = 0,
        .reservedRangeSize = 0,
    };
}

} // namespace Vulkan
