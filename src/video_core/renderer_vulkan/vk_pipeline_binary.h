// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <vector>

#include "video_core/renderer_vulkan/pipeline_binary_store.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

/// The driver's binaries of the pipelines Lichii builds (VK_KHR_pipeline_binary). A pipeline built
/// for the first time is captured and its binaries are stored with PipelineBinaryStore; the next
/// time the same pipeline is created (a later boot, or after it was evicted) it is created from
/// them and the driver compiles nothing. This does not depend on the driver's own cache, which a
/// driver may trim or drop. Used only where Lichii passes no VkPipelineCache (pipeline binaries
/// and pipeline caches can't be combined in one creation).
class PipelineBinaryCache {
public:
    explicit PipelineBinaryCache(const Device& device);

    /// Opens the title's binary file; pipelines are created the plain way until this succeeds.
    void Open(const std::filesystem::path& file);

    [[nodiscard]] vk::Pipeline CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo& ci,
                                                      VkPipelineCache cache);

    [[nodiscard]] vk::Pipeline CreateComputePipeline(const VkComputePipelineCreateInfo& ci,
                                                     VkPipelineCache cache);

    /// Logs the counters of the last 10 s (Nx2Perf pipeline_binary), when there were any.
    void Report();

private:
    template <typename CreateInfo, typename Create>
    vk::Pipeline CreatePipeline(const CreateInfo& ci, VkPipelineCache cache, Create&& create);

    template <typename CreateInfo, typename Create>
    vk::Pipeline CreateFromBinaries(const CreateInfo& ci,
                                    const std::vector<PipelineBinaryBlob>& blobs, Create&& create);

    void Capture(VkPipeline pipeline, const PipelineBinaryKey& key);

    const Device& device;
    const bool supported;
    std::atomic_bool open{};
    PipelineBinaryStore store;

    std::atomic<u32> created_from_binaries{};
    std::atomic<u32> captured{};
    std::atomic<u32> rejected{};
    std::atomic<u32> failed_captures{};
    std::atomic<u64> binary_ns{};
    std::atomic<u64> capture_ns{};
};

} // namespace Vulkan
