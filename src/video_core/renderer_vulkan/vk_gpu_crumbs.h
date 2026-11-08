// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/gpu_crumb_log.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

/// EDEN_NX2_GPU_CRUMBS: AMD buffer markers around every submit, render pass and guest dispatch
/// (=2: every draw too). The GPU writes the id of the newest started (top of pipe) and finished
/// (bottom of pipe) crumb of each kind to host-visible, device-coherent, uncached memory, which
/// survives a device loss; Report() names the last finished and the first unfinished commands.
/// All recording runs on the scheduler's worker thread, in command order.
class GpuCrumbTrail {
public:
    /// Null unless EDEN_NX2_GPU_CRUMBS is set and the device has VK_AMD_buffer_marker with
    /// device-coherent memory.
    [[nodiscard]] static std::unique_ptr<GpuCrumbTrail> Create(const Device& device);

    GpuCrumbTrail(const Device& device, VkBuffer buffer, vk::DeviceMemory memory,
                  volatile u32* markers);
    ~GpuCrumbTrail();
    GpuCrumbTrail(const GpuCrumbTrail&) = delete;
    GpuCrumbTrail& operator=(const GpuCrumbTrail&) = delete;

    void BeginSubmit(vk::CommandBuffer cmdbuf);
    void EndSubmit(vk::CommandBuffer cmdbuf, u64 tick);
    void BeginPass(vk::CommandBuffer cmdbuf, u32 width, u32 height);
    void EndPass(vk::CommandBuffer cmdbuf);
    /// A guest dispatch (outside a pass) or a draw (inside one; marked at level 2 only).
    /// Returns the id to end it with, 0 when it is not marked.
    u32 BeginWork(vk::CommandBuffer cmdbuf, u64 hash, u64 fragment_hash, std::array<u32, 3> size);
    void EndWork(vk::CommandBuffer cmdbuf, u32 id);

    /// Logs the crumbs as the GPU last wrote them (at a device loss).
    void Report() const;

private:
    void Mark(vk::CommandBuffer cmdbuf, CrumbKind kind, bool finished, u32 id) const;

    const Device& device;
    VkBuffer buffer;
    vk::DeviceMemory memory;
    volatile u32* markers;
    bool mark_draws;
    CrumbLog log;
    u32 submit_id{};
    u32 submit_count{};
    u32 pass_id{};
};

} // namespace Vulkan
