// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <span>
#include <vector>

#include "common/fs/fs_util.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_pipeline_binary.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {
PipelineBinaryKey ToKey(const VkPipelineBinaryKeyKHR& key) {
    PipelineBinaryKey result;
    result.size = std::min<u32>(key.keySize, PipelineBinaryKey::MaxSize);
    std::copy_n(key.key, result.size, result.bytes.begin());
    return result;
}

VkPipelineBinaryKeyKHR ToVulkan(const PipelineBinaryKey& key) {
    VkPipelineBinaryKeyKHR result{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR,
        .pNext = nullptr,
        .keySize = key.size,
        .key = {},
    };
    std::copy_n(key.bytes.begin(), key.size, result.key);
    return result;
}

u64 Since(std::chrono::steady_clock::time_point begin) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - begin)
                                .count());
}

void DestroyBinaries(const Device& device, std::span<const VkPipelineBinaryKHR> binaries) {
    const auto& dld = device.GetDispatchLoader();
    for (const VkPipelineBinaryKHR binary : binaries) {
        if (binary != VK_NULL_HANDLE) {
            dld.vkDestroyPipelineBinaryKHR(*device.GetLogical(), binary, nullptr);
        }
    }
}
} // Anonymous namespace

PipelineBinaryCache::PipelineBinaryCache(const Device& device_)
    : device{device_}, supported{device_.IsKhrPipelineBinarySupported()} {}

void PipelineBinaryCache::Open(const std::filesystem::path& file) {
    open = false;
    if (!supported) {
        return;
    }
    VkPipelineBinaryKeyKHR global_key{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR,
        .pNext = nullptr,
        .keySize = 0,
        .key = {},
    };
    const auto& dld = device.GetDispatchLoader();
    // A null create info gives the driver's global key: it changes with the driver or device.
    if (dld.vkGetPipelineKeyKHR(*device.GetLogical(), nullptr, &global_key) != VK_SUCCESS) {
        LOG_WARNING(Render_Vulkan, "Pipeline binaries: no global pipeline key, not used");
        return;
    }
    if (!store.Open(file, ToKey(global_key))) {
        LOG_WARNING(Render_Vulkan, "Pipeline binaries: can't open {}",
                    Common::FS::PathToUTF8String(file));
        return;
    }
    open = true;
    LOG_INFO(Render_Vulkan, "Pipeline binaries: {} pipelines in {}", store.Size(),
             Common::FS::PathToUTF8String(file));
}

vk::Pipeline PipelineBinaryCache::CreateGraphicsPipeline(const VkGraphicsPipelineCreateInfo& ci,
                                                         VkPipelineCache cache) {
    return CreatePipeline(
        ci, cache,
        [this](const VkGraphicsPipelineCreateInfo& info, VkPipelineCache pipeline_cache) {
            return device.GetLogical().CreateGraphicsPipeline(info, pipeline_cache);
        });
}

vk::Pipeline PipelineBinaryCache::CreateComputePipeline(const VkComputePipelineCreateInfo& ci,
                                                        VkPipelineCache cache) {
    return CreatePipeline(
        ci, cache, [this](const VkComputePipelineCreateInfo& info, VkPipelineCache pipeline_cache) {
            return device.GetLogical().CreateComputePipeline(info, pipeline_cache);
        });
}

template <typename CreateInfo, typename Create>
vk::Pipeline PipelineBinaryCache::CreatePipeline(const CreateInfo& ci, VkPipelineCache cache,
                                                 Create&& create) {
    // Libraries are linked, not stored; a pipeline cache can't be combined with binaries.
    if (!open.load(std::memory_order_relaxed) || cache != VK_NULL_HANDLE ||
        (ci.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0) {
        return create(ci, cache);
    }
    const auto& dld = device.GetDispatchLoader();
    // VkPipelineCreateInfoKHR::pNext is a non-const pointer; the driver only reads through it.
    const VkPipelineCreateInfoKHR key_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR,
        .pNext = const_cast<CreateInfo*>(&ci),
    };
    VkPipelineBinaryKeyKHR vk_key{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR,
        .pNext = nullptr,
        .keySize = 0,
        .key = {},
    };
    if (dld.vkGetPipelineKeyKHR(*device.GetLogical(), &key_ci, &vk_key) != VK_SUCCESS) {
        return create(ci, cache);
    }
    const PipelineBinaryKey key = ToKey(vk_key);
    if (const auto blobs = store.Find(key)) {
        const auto begin = std::chrono::steady_clock::now();
        vk::Pipeline pipeline = CreateFromBinaries(ci, *blobs, create);
        if (pipeline) {
            created_from_binaries.fetch_add(1, std::memory_order_relaxed);
            binary_ns.fetch_add(Since(begin), std::memory_order_relaxed);
            return pipeline;
        }
        // Rejected by the driver: built again below, and its new binaries replace these.
        rejected.fetch_add(1, std::memory_order_relaxed);
    }
    // With VkPipelineCreateFlags2CreateInfo the driver reads the flags from there only.
    const VkPipelineCreateFlags2CreateInfoKHR flags2{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
        .pNext = ci.pNext,
        .flags = static_cast<VkPipelineCreateFlags2KHR>(ci.flags) |
                 VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR,
    };
    CreateInfo capture_ci = ci;
    capture_ci.pNext = &flags2;
    vk::Pipeline pipeline = create(capture_ci, VK_NULL_HANDLE);
    const auto begin = std::chrono::steady_clock::now();
    Capture(*pipeline, key);
    capture_ns.fetch_add(Since(begin), std::memory_order_relaxed);
    return pipeline;
}

template <typename CreateInfo, typename Create>
vk::Pipeline PipelineBinaryCache::CreateFromBinaries(const CreateInfo& ci,
                                                     const std::vector<PipelineBinaryBlob>& blobs,
                                                     Create&& create) {
    const auto& dld = device.GetDispatchLoader();
    std::vector<VkPipelineBinaryKeyKHR> keys;
    std::vector<VkPipelineBinaryDataKHR> data;
    keys.reserve(blobs.size());
    data.reserve(blobs.size());
    for (const PipelineBinaryBlob& blob : blobs) {
        keys.push_back(ToVulkan(blob.key));
        data.push_back({
            .dataSize = blob.data.size(),
            .pData = const_cast<u8*>(blob.data.data()),
        });
    }
    const VkPipelineBinaryKeysAndDataKHR keys_and_data{
        .binaryCount = static_cast<u32>(blobs.size()),
        .pPipelineBinaryKeys = keys.data(),
        .pPipelineBinaryData = data.data(),
    };
    const VkPipelineBinaryCreateInfoKHR binary_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
        .pNext = nullptr,
        .pKeysAndDataInfo = &keys_and_data,
        .pipeline = VK_NULL_HANDLE,
        .pPipelineCreateInfo = nullptr,
    };
    std::vector<VkPipelineBinaryKHR> binaries(blobs.size(), VK_NULL_HANDLE);
    VkPipelineBinaryHandlesInfoKHR handles{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
        .pNext = nullptr,
        .pipelineBinaryCount = static_cast<u32>(binaries.size()),
        .pPipelineBinaries = binaries.data(),
    };
    if (dld.vkCreatePipelineBinariesKHR(*device.GetLogical(), &binary_ci, nullptr, &handles) !=
        VK_SUCCESS) {
        DestroyBinaries(device, binaries);
        return {};
    }
    const VkPipelineBinaryInfoKHR binary_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,
        .pNext = ci.pNext,
        .binaryCount = static_cast<u32>(binaries.size()),
        .pPipelineBinaries = binaries.data(),
    };
    CreateInfo binary_pipeline_ci = ci;
    binary_pipeline_ci.pNext = &binary_info;
    vk::Pipeline pipeline;
    try {
        pipeline = create(binary_pipeline_ci, VK_NULL_HANDLE);
    } catch (const vk::Exception& exception) {
        LOG_DEBUG(Render_Vulkan, "Pipeline binaries: creation from binaries failed: {}",
                  exception.what());
    }
    // A pipeline doesn't keep the binaries it was created from.
    DestroyBinaries(device, binaries);
    return pipeline;
}

void PipelineBinaryCache::Capture(VkPipeline pipeline, const PipelineBinaryKey& key) {
    const auto& dld = device.GetDispatchLoader();
    const VkDevice handle = *device.GetLogical();
    const VkPipelineBinaryCreateInfoKHR binary_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
        .pNext = nullptr,
        .pKeysAndDataInfo = nullptr,
        .pipeline = pipeline,
        .pPipelineCreateInfo = nullptr,
    };
    VkPipelineBinaryHandlesInfoKHR handles{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
        .pNext = nullptr,
        .pipelineBinaryCount = 0,
        .pPipelineBinaries = nullptr,
    };
    std::vector<VkPipelineBinaryKHR> binaries;
    bool ok =
        dld.vkCreatePipelineBinariesKHR(handle, &binary_ci, nullptr, &handles) == VK_SUCCESS &&
        handles.pipelineBinaryCount != 0 &&
        handles.pipelineBinaryCount <= PipelineBinaryStore::MaxBinariesPerPipeline;
    if (ok) {
        binaries.assign(handles.pipelineBinaryCount, VK_NULL_HANDLE);
        handles.pPipelineBinaries = binaries.data();
        ok = dld.vkCreatePipelineBinariesKHR(handle, &binary_ci, nullptr, &handles) == VK_SUCCESS;
    }
    std::vector<PipelineBinaryBlob> blobs;
    for (size_t i = 0; ok && i < binaries.size(); ++i) {
        const VkPipelineBinaryDataInfoKHR data_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR,
            .pNext = nullptr,
            .pipelineBinary = binaries[i],
        };
        VkPipelineBinaryKeyKHR binary_key{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR,
            .pNext = nullptr,
            .keySize = 0,
            .key = {},
        };
        size_t size = 0;
        ok = dld.vkGetPipelineBinaryDataKHR(handle, &data_info, &binary_key, &size, nullptr) ==
                 VK_SUCCESS &&
             size != 0 && size <= PipelineBinaryStore::MaxDataSize;
        if (!ok) {
            break;
        }
        PipelineBinaryBlob blob{.key = ToKey(binary_key), .data = std::vector<u8>(size)};
        ok = dld.vkGetPipelineBinaryDataKHR(handle, &data_info, &binary_key, &size,
                                            blob.data.data()) == VK_SUCCESS;
        blob.data.resize(size);
        blobs.push_back(std::move(blob));
    }
    // The pipeline keeps no capture data from here on.
    const VkReleaseCapturedPipelineDataInfoKHR release_info{
        .sType = VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR,
        .pNext = nullptr,
        .pipeline = pipeline,
    };
    dld.vkReleaseCapturedPipelineDataKHR(handle, &release_info, nullptr);
    DestroyBinaries(device, binaries);
    if (ok && store.Store(key, blobs)) {
        captured.fetch_add(1, std::memory_order_relaxed);
    } else {
        failed_captures.fetch_add(1, std::memory_order_relaxed);
    }
}

void PipelineBinaryCache::Report() {
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (!open.load(std::memory_order_relaxed) || now - window < std::chrono::seconds{10}) {
        return;
    }
    window = now;
    const auto take = [](auto& counter) { return counter.exchange(0, std::memory_order_relaxed); };
    const u32 from_binaries = take(created_from_binaries);
    const u32 new_captures = take(captured);
    const u32 rejections = take(rejected);
    const u32 failures = take(failed_captures);
    const u64 binary_time = take(binary_ns);
    const u64 capture_time = take(capture_ns);
    if (from_binaries == 0 && new_captures == 0 && rejections == 0 && failures == 0) {
        return;
    }
    const auto ms = [](u64 ns) { return static_cast<double>(ns) / 1'000'000.0; };
    LOG_INFO(Render_Vulkan,
             "Nx2Perf pipeline_binary in the last 10 s: {} created from stored binaries ({:.1f} "
             "ms), {} captured ({:.1f} ms of capture), {} rejected by the driver, {} captures "
             "failed; {} pipelines stored",
             from_binaries, ms(binary_time), new_captures, ms(capture_time), rejections, failures,
             store.Size());
}

} // namespace Vulkan
