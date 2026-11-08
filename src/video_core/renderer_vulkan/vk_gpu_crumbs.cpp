// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <optional>

#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_gpu_crumbs.h"
#include "video_core/vulkan_common/gpu_crumbs.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

constexpr VkDeviceSize MarkerBytes = NumCrumbKinds * 2 * sizeof(u32);

/// Host visible and coherent, and device coherent so the writes reach memory as they happen
/// (VK_AMD_device_coherent_memory); uncached preferred, as AMD's crash marker samples use.
std::optional<u32> FindMarkerMemoryType(const Device& device, u32 type_bits) {
    const VkPhysicalDeviceMemoryProperties properties =
        device.GetPhysical().GetMemoryProperties().memoryProperties;
    constexpr VkMemoryPropertyFlags required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                               VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
    std::optional<u32> found;
    for (u32 index = 0; index < properties.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags = properties.memoryTypes[index].propertyFlags;
        if ((type_bits & (1u << index)) == 0 || (flags & required) != required) {
            continue;
        }
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD) != 0) {
            return index;
        }
        if (!found) {
            found = index;
        }
    }
    return found;
}

} // Anonymous namespace

std::unique_ptr<GpuCrumbTrail> GpuCrumbTrail::Create(const Device& device) {
    if (!GpuCrumbs::Requested()) {
        return nullptr;
    }
    if (!device.IsBufferMarkerEnabled()) {
        LOG_WARNING(Render_Vulkan, "GPU crumbs: no VK_AMD_buffer_marker with device-coherent "
                                   "memory; device losses name no GPU command");
        return nullptr;
    }
    const auto& dld = device.GetDispatchLoader();
    const VkDevice logical = *device.GetLogical();
    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = MarkerBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    VkBuffer buffer{};
    vk::Check(dld.vkCreateBuffer(logical, &buffer_ci, nullptr, &buffer));
    const VkMemoryRequirements requirements =
        device.GetLogical().GetBufferMemoryRequirements(buffer);
    const std::optional<u32> type = FindMarkerMemoryType(device, requirements.memoryTypeBits);
    if (!type) {
        dld.vkDestroyBuffer(logical, buffer, nullptr);
        LOG_WARNING(Render_Vulkan, "GPU crumbs: no device-coherent host-visible memory type");
        return nullptr;
    }
    vk::DeviceMemory memory = device.GetLogical().AllocateMemory({
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = requirements.size,
        .memoryTypeIndex = *type,
    });
    vk::Check(dld.vkBindBufferMemory(logical, buffer, *memory, 0));
    auto* const markers = reinterpret_cast<volatile u32*>(memory.Map(0, MarkerBytes));
    for (size_t i = 0; i < NumCrumbKinds * 2; ++i) {
        markers[i] = 0;
    }
    LOG_INFO(Render_Vulkan, "GPU crumbs: buffer markers in memory type {}{}", *type,
             GpuCrumbs::Level() >= 2 ? ", every draw marked" : "");
    return std::make_unique<GpuCrumbTrail>(device, buffer, std::move(memory), markers);
}

GpuCrumbTrail::GpuCrumbTrail(const Device& device_, VkBuffer buffer_, vk::DeviceMemory memory_,
                             volatile u32* markers_)
    : device{device_}, buffer{buffer_}, memory{std::move(memory_)}, markers{markers_},
      mark_draws{GpuCrumbs::Level() >= 2} {}

GpuCrumbTrail::~GpuCrumbTrail() {
    memory.Unmap();
    device.GetDispatchLoader().vkDestroyBuffer(*device.GetLogical(), buffer, nullptr);
}

void GpuCrumbTrail::Mark(vk::CommandBuffer cmdbuf, CrumbKind kind, bool finished, u32 id) const {
    const VkDeviceSize offset = (static_cast<size_t>(kind) * 2 + (finished ? 1 : 0)) * sizeof(u32);
    cmdbuf.WriteBufferMarkerAMD(finished ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                buffer, offset, id);
}

void GpuCrumbTrail::BeginSubmit(vk::CommandBuffer cmdbuf) {
    submit_id = log.Add({.kind = CrumbKind::Submit, .submit = ++submit_count});
    Mark(cmdbuf, CrumbKind::Submit, false, submit_id);
}

void GpuCrumbTrail::EndSubmit(vk::CommandBuffer cmdbuf, u64 tick) {
    log.SetSubmitTick(submit_id, tick);
    Mark(cmdbuf, CrumbKind::Submit, true, submit_id);
}

void GpuCrumbTrail::BeginPass(vk::CommandBuffer cmdbuf, u32 width, u32 height) {
    pass_id =
        log.Add({.kind = CrumbKind::Pass, .submit = submit_count, .size = {width, height, 1}});
    Mark(cmdbuf, CrumbKind::Pass, false, pass_id);
}

void GpuCrumbTrail::EndPass(vk::CommandBuffer cmdbuf) {
    if (pass_id == 0) {
        return;
    }
    Mark(cmdbuf, CrumbKind::Pass, true, pass_id);
    pass_id = 0;
}

u32 GpuCrumbTrail::BeginWork(vk::CommandBuffer cmdbuf, u64 hash, u64 fragment_hash,
                             std::array<u32, 3> size) {
    const bool draw = pass_id != 0;
    if (draw && !mark_draws) {
        return 0;
    }
    const u32 id = log.Add({.kind = CrumbKind::Work,
                            .draw = draw,
                            .submit = submit_count,
                            .a = hash,
                            .b = fragment_hash,
                            .size = size});
    Mark(cmdbuf, CrumbKind::Work, false, id);
    return id;
}

void GpuCrumbTrail::EndWork(vk::CommandBuffer cmdbuf, u32 id) {
    if (id != 0) {
        Mark(cmdbuf, CrumbKind::Work, true, id);
    }
}

void GpuCrumbTrail::Report() const {
    std::array<std::array<u32, 2>, NumCrumbKinds> values{};
    for (size_t kind = 0; kind < NumCrumbKinds; ++kind) {
        values[kind] = {markers[kind * 2], markers[kind * 2 + 1]};
    }
    for (const std::string& line : log.Report(values)) {
        LOG_CRITICAL(Render_Vulkan, "{}", line);
    }
}

} // namespace Vulkan
