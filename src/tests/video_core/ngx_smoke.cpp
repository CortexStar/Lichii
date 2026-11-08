// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Optional hardware test, deliberately excluded from ctest. Does not boot the emulator.
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/neural_rendering/runtime.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
#include "video_core/renderer_vulkan/vk_nvidia_dlss.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/ngx_dlss.h"
#include "video_core/vulkan_common/ngx_api.h"
#include "video_core/vulkan_common/nvidia_dlss.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace {
using namespace Vulkan;

void Require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct InputImage {
    vk::Image image;
    vk::ImageView view;
    NvidiaDlssUpscaler::Image resource;
};

InputImage MakeInput(const Device &device, MemoryAllocator &allocator, VkFormat format,
                     VkImageAspectFlags aspect, VkExtent2D extent) {
    InputImage result;
    result.image = allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    const VkImageSubresourceRange range{aspect, 0, 1, 0, 1};
    result.view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *result.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = range,
    });
    result.resource = {*result.view, *result.image, format, range, aspect};
    return result;
}

void RenderCases(const Device& device, u32 scale, bool expect_neural) {
    MemoryAllocator allocator{device};
    StateTracker tracker;
    Scheduler scheduler{device, tracker};
    NvidiaDlssUpscaler upscaler{device, scheduler, allocator};
    const VkExtent2D input_extent{320 * scale, 180 * scale};
    auto color = MakeInput(device, allocator, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_ASPECT_COLOR_BIT, input_extent);
    auto depth = MakeInput(device, allocator, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_ASPECT_DEPTH_BIT, input_extent);
    auto motion = MakeInput(device, allocator, VK_FORMAT_R16G16_SFLOAT,
                            VK_IMAGE_ASPECT_COLOR_BIT, input_extent);
    auto alternate_motion_view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *motion.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = motion.resource.format,
        .subresourceRange = motion.resource.range,
    });
    const auto original_motion_view = motion.resource.view;
    auto &api = const_cast<Ngx::Api &>(Ngx::GetApi());
    const auto evaluate = api.evaluate;
    SCOPE_EXIT { api.evaluate = evaluate; };
    constexpr std::array outputs{VkExtent2D{480, 270}, VkExtent2D{480, 270}, VkExtent2D{480, 270},
                                 VkExtent2D{320, 180}, VkExtent2D{480, 270}, VkExtent2D{480, 270},
                                 VkExtent2D{480, 270}};
    for (size_t frame = 0; frame < outputs.size(); ++frame) {
        // Rebind while temporal history is live, without resizing the backend.
        motion.resource.view = frame == 1 ? *alternate_motion_view : original_motion_view;
        const VkExtent2D extent{outputs[frame].width * scale, outputs[frame].height * scale};
        const bool inject_failure = frame == outputs.size() - 1;
        if (inject_failure) {
            api.evaluate = [](VkCommandBuffer, const NVSDK_NGX_Handle *,
                              const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback) {
                return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
            };
        }
        const std::array<float, 4> expected = (frame < 2 || frame % 2 == 0)
                                                  ? std::array{0.25f, 0.5f, 0.75f, 1.0f}
                                                  : std::array{0.75f, 0.25f, 0.5f, 1.0f};
        scheduler.Record([&, frame, expected](vk::CommandBuffer cmd) {
            for (const auto &image : {color.resource, depth.resource, motion.resource}) {
                cmd.PipelineBarrier(
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = frame == 0 ? 0u : VK_ACCESS_MEMORY_READ_BIT,
                        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                        .oldLayout =
                            frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image.image,
                        .subresourceRange = image.range,
                    });
            }
            VkClearColorValue clear{};
            std::copy(expected.begin(), expected.end(), clear.float32);
            cmd.ClearColorImage(*color.image, VK_IMAGE_LAYOUT_GENERAL, clear, color.resource.range);
            cmd.ClearColorImage(*motion.image, VK_IMAGE_LAYOUT_GENERAL, {}, motion.resource.range);
            const VkClearDepthStencilValue clear_depth{0.5f, 0};
            const auto clear_fn = reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(
                device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                               "vkCmdClearDepthStencilImage"));
            clear_fn(*cmd, *depth.image, VK_IMAGE_LAYOUT_GENERAL, &clear_depth, 1,
                     &depth.resource.range);
        });
        Require(upscaler.Upscale({color.resource,
                                  depth.resource,
                                  motion.resource,
                                  input_extent,
                                  extent,
                                  {0, 0},
                                  {1, 1},
                                  frame != 1,
                                  frame != 5,
                                  frame == 5}),
                "Upscale refused a frame");
        auto readback = allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = VkDeviceSize{extent.width} * extent.height * 8,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Download);
        scheduler.Record([&, extent](vk::CommandBuffer cmd) {
            cmd.PipelineBarrier(
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT});
            cmd.CopyImageToBuffer(
                upscaler.OutputImage(), VK_IMAGE_LAYOUT_GENERAL, *readback,
                VkBufferImageCopy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {extent.width, extent.height, 1}});
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                                VkMemoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                                VK_ACCESS_HOST_READ_BIT});
        });
        scheduler.Finish();
        api.evaluate = evaluate;
        Require(upscaler.Failed() == inject_failure, "Unexpected NGX evaluation result");
        if (inject_failure) {
            Require(!Ngx::GetStatus().error.empty(), "Evaluation failure was not diagnosed");
            Require(!upscaler.Upscale({}), "Failed upscaler did not request fallback");
        }
        readback.Invalidate();
        const auto bytes = readback.Mapped();
        std::array<double, 3> sums{};
        const size_t pixels = size_t{extent.width} * extent.height;
        for (size_t i = 0; i < pixels; ++i) {
            for (size_t c = 0; c < sums.size(); ++c) {
                _Float16 half;
                std::memcpy(&half, bytes.data() + (i * 4 + c) * 2, sizeof(half));
                const float value = static_cast<float>(half);
                Require(std::isfinite(value), "Non-finite output");
                sums[c] += value;
            }
        }
        for (size_t c = 0; c < sums.size(); ++c) {
            if (!expect_neural || inject_failure) {
                Require(std::abs(sums[c] / pixels - (inject_failure ? 0.0f : expected[c])) < 0.025,
                        "Readback differs from input; stale or incorrect output");
            } else {
                Require(sums[c] / pixels > 0.01 && sums[c] / pixels < 16.0,
                        "Neural output is empty or outside the expected HDR range");
            }
        }
        std::printf("PASS frame %zu: %ux%u -> %ux%u; RGB %.4f %.4f %.4f\n", frame,
                    input_extent.width, input_extent.height, extent.width, extent.height,
                    sums[0] / pixels, sums[1] / pixels, sums[2] / pixels);
    }
    Require(Ngx::GetStatus().evaluations == outputs.size() - 1, "Incorrect evaluation count");
    const auto neural = VideoCore::NeuralRendering::GetStatus();
    Require(neural.submitted_frames == (expect_neural ? outputs.size() - 1 : 0),
            "Incorrect neural rendering submission count");
}
} // namespace

int main(int argc, char **argv) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::fprintf(stderr, "Usage: ngx_smoke <Linux DLSS directory> [GPU index] [scale 1-8] [NR "
                             "model directory or missing]\n");
        return 2;
    }
    const auto scratch =
        std::filesystem::temp_directory_path() / ("lichii-ngx-smoke-" + std::to_string(getpid()));
    std::filesystem::create_directories(scratch / "log");
    std::filesystem::create_directories(scratch / "cache");
    Common::FS::SetEdenPath(Common::FS::EdenPath::EdenDir, scratch);
    Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, scratch / "cache");
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, scratch / "log");
    Common::Log::Initialize();
    Common::Log::Start();
    Settings::values.nvidia_dlss = false;
    Require(!NvidiaDlss::Start() && !NvidiaDlss::WantsDevice(), "Disabled setting loaded NGX");
    Settings::values.nvidia_dlss = true;
    setenv("LICHII_NGX_DIR", scratch.c_str(), 1);
    Require(!NvidiaDlss::Start() && !Ngx::GetStatus().error.empty(), "Missing model not diagnosed");
    setenv("LICHII_NGX_DIR", std::filesystem::absolute(argv[1]).c_str(), 1);
    const size_t gpu = argc > 2 ? std::stoul(argv[2]) : 0;
    const auto scale = argc > 3 ? std::stoul(argv[3]) : 1;
    const bool request_neural = argc > 4;
    const bool expect_neural = request_neural && std::string_view{argv[4]} != "missing";
    Settings::values.neural_rendering =
        request_neural ? Settings::NeuralRendering::Natural : Settings::NeuralRendering::Off;
    if (expect_neural) {
        const auto target = VideoCore::NeuralRendering::UserFolder();
        std::filesystem::create_directories(target.parent_path());
        std::filesystem::create_directory_symlink(std::filesystem::absolute(argv[4]), target);
    }
    Require(scale >= 1 && scale <= 8, "Scale must be between 1 and 8 (4K output)");
    for (int run = 0; run < 2; ++run) {
        Require(NvidiaDlss::Start(), "NGX startup failed");
        {
            auto library = OpenLibrary();
            vk::InstanceDispatch dispatch;
            auto instance = CreateInstance(*library, dispatch, VK_API_VERSION_1_3,
                                           Core::Frontend::WindowSystemType::X11);
            const auto physical_devices = instance.EnumeratePhysicalDevices();
            Require(gpu < physical_devices.size(), "GPU index out of range");
            Device device{*instance, vk::PhysicalDevice{physical_devices[gpu], dispatch},
                          VK_NULL_HANDLE, dispatch};
            Require(NvidiaDlss::IsAvailable(), "Native DLSS unavailable on selected device");
            Require(!request_neural || device.SupportsNeuralRendering(),
                    "Neural rendering device features were not enabled");
            Require(!NvidiaDlss::WantsDevice(), "Attached runtime still requests a device");
            auto duplicate_dispatch = device.GetDispatchLoader();
            Require(!NvidiaDlss::AttachDevice(*instance, physical_devices[gpu],
                                              *device.GetLogical(), device.GetGraphicsFamily(),
                                              duplicate_dispatch),
                    "NGX accepted a second device attachment");
            Require(NvidiaDlss::IsAvailable(), "Duplicate attachment disturbed the active device");
            RenderCases(device, static_cast<u32>(scale), expect_neural);
            if (request_neural && !expect_neural) {
                Require(!VideoCore::NeuralRendering::GetStatus().error.empty(),
                        "Missing neural model was not diagnosed");
            }
        }
        Require(!NvidiaDlss::IsAvailable(), "Device teardown left NGX active");
        std::printf("PASS device lifecycle %d\n", run + 1);
    }
    NvidiaDlss::Stop();
    std::printf("Logs/cache: %s\n", scratch.c_str());
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
