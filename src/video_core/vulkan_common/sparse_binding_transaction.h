// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <utility>
#include <vector>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

template <typename Allocator>
class SparseBindingTransaction {
public:
    SparseBindingTransaction(Allocator& allocator_, VkDeviceSize& bound_bytes_)
        : allocator{allocator_}, bound_bytes{bound_bytes_} {}

    SparseBindingTransaction(const SparseBindingTransaction&) = delete;
    SparseBindingTransaction& operator=(const SparseBindingTransaction&) = delete;

    ~SparseBindingTransaction() {
        for (const auto& [allocation, size] : fresh) {
            allocator.FreeSparseMemory(*allocation);
            *allocation = VK_NULL_HANDLE;
            bound_bytes -= size;
        }
    }

    bool Allocate(const VkMemoryRequirements& requirements, VkDeviceMemory& memory,
                  VkDeviceSize& offset, VmaAllocation& allocation) {
        fresh.emplace_back(&allocation, requirements.size);
        allocation = allocator.AllocateSparseMemory(requirements, memory, offset);
        if (allocation == VK_NULL_HANDLE) {
            fresh.pop_back();
            return false;
        }
        bound_bytes += requirements.size;
        return true;
    }

    void Commit() {
        fresh.clear();
    }

private:
    Allocator& allocator;
    VkDeviceSize& bound_bytes;
    std::vector<std::pair<VmaAllocation*, VkDeviceSize>> fresh;
};

} // namespace Vulkan
