// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class DescriptorPool;
class Device;
class MemoryAllocator;
class Scheduler;

struct OcarinaActorMesh {
    std::vector<u8> vertices;
    std::vector<u16> indices;
    std::array<f32, 4> current_origin;
    std::array<f32, 4> previous_origin;
    std::array<f32, 280 * 12> current_bones;
    std::array<f32, 280 * 12> previous_bones;
};

#ifdef EGLANTINE_STREAMLINE
class OcarinaLiveDlss {
public:
    OcarinaLiveDlss(const Device& device, Scheduler& scheduler, DescriptorPool& pool,
                    ComputePassDescriptorQueue& queue, MemoryAllocator& allocator);
    ~OcarinaLiveDlss();

    [[nodiscard]] bool EvaluationFailed() const;
    void SaveSceneReference(VkImageView color);
    void Composite4k(VkImageView native_composite, u64 frame);
    VkImage CompositedImage() const;
    VkImageView CompositedView() const;

    bool Record(VkImageView color, VkImageView depth, VkImageView target, VkExtent2D extent,
                VkExtent2D output_extent, VkExtent2D target_extent, const std::array<f32, 16>& current,
                const std::array<f32, 16>& previous, const OcarinaActorMesh& mesh,
                VkImage depth_image, u64 frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif

} // namespace Vulkan
