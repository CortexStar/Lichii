// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include "video_core/draw_class_stats.h"
#include "video_core/draw_input_watch.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"

#include "common/assert.h"
#include "common/div_ceil.h"
#include "common/dkb_frame_trace.h"
#include "common/logging.h"
#include "common/nx2_switches.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "shader_recompiler/exception.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/control/channel_state.h"
#include "video_core/draw_debug_switches.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host_logic_op.h"
#include "video_core/indirect_reset.h"
#include "video_core/memory_manager.h"
#include "video_core/native_indirect_reset.h"
#include "video_core/native_query_trace.h"
#include "video_core/nx2.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_reactive.h"
#include "video_core/oot_dlss.h"
#include "video_core/render_trace.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/framebuffer_clear_layers.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/submission_policy.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_oot_object_motion.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/textures/texture.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using VideoCommon::ImageViewId;
using VideoCommon::ImageViewType;


namespace {

/// read_gpu_memory: the first address space (the bound channel's first) that maps the whole range,
/// read as the GPU reads it, without flushing GPU caches.
VideoCore::Nx2Debug::GpuReadResult ReadGpuMemory(std::span<Tegra::MemoryManager* const> spaces,
                                                 GPUVAddr address, u64 size) {
    VideoCore::Nx2Debug::GpuReadResult result{};
    if (spaces.empty()) {
        result.error = "no GPU address space is bound yet";
        return result;
    }
    for (Tegra::MemoryManager* const space : spaces) {
        if (space->IsFullyMappedRange(address, size)) {
            result.bytes.resize(size);
            space->ReadBlockUnsafe(address, result.bytes.data(), size);
            result.address_space = space->GetID();
            return result;
        }
    }
    constexpr u64 page_mask = 0xfff;
    const GPUVAddr end = address + size;
    GPUVAddr first = address;
    while (first < end) {
        const GPUVAddr next = (std::min)((first | page_mask) + 1, end);
        if (!spaces.front()->IsFullyMappedRange(first, next - first)) {
            break;
        }
        first = next;
    }
    result.error = fmt::format("GPU address {:#x} is not mapped (the first unmapped byte in the "
                               "bound channel's address space; {} address spaces checked)",
                               first, spaces.size());
    return result;
}

struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

VkViewport GetViewportState(const Device& device, const Maxwell& regs, size_t index, float scale) {
    const auto& src = regs.viewport_transform[index];
    const auto conv = [scale](float value) {
        float new_value = value * scale;
        if (scale < 1.0f) {
            const bool sign = std::signbit(value);
            new_value = std::round(std::abs(new_value));
            new_value = sign ? -new_value : new_value;
        }
        return new_value;
    };
    const float x = conv(src.translate_x - src.scale_x);
    const float width = conv(src.scale_x * 2.0f);
    float y = conv(src.translate_y - src.scale_y);
    float height = conv(src.scale_y * 2.0f);

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const bool y_negate = !device.IsNvViewportSwizzleSupported() &&
                          src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY;

    if (lower_left) {
        // Flip by surface clip height
        y += conv(static_cast<f32>(regs.surface_clip.height));
        height = -height;
    }

    if (y_negate) {
        // Flip by viewport height
        y += height;
        height = -height;
    }

    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    VkViewport viewport{
        .x = x,
        .y = y,
        .width = width != 0.0f ? width : 1.0f,
        .height = height != 0.0f ? height : 1.0f,
        .minDepth = src.translate_z - src.scale_z * reduce_z,
        .maxDepth = src.translate_z + src.scale_z,
    };
    if (!device.IsExtDepthRangeUnrestrictedSupported()) {
        viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
    }
    return viewport;
}

VkRect2D GetScissorState(const Maxwell& regs, size_t index, u32 up_scale = 1, u32 down_shift = 0) {
    const auto& src = regs.scissor_test[index];
    VkRect2D scissor{};
    const auto scale_up = [&](s32 value) -> s32 {
        if (value == 0) {
            return 0U;
        }
        const s32 upset = value * up_scale;
        s32 acumm = 0;
        if ((up_scale >> down_shift) == 0) {
            acumm = upset % 2;
        }
        const s32 converted_value = (value * up_scale) >> down_shift;
        return value < 0 ? std::min<s32>(converted_value - acumm, -1)
                         : std::max<s32>(converted_value + acumm, 1);
    };

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = regs.surface_clip.height;

    // Flip coordinates if lower left
    s32 min_y = lower_left ? (clip_height - src.max_y) : src.min_y.Value();
    s32 max_y = lower_left ? (clip_height - src.min_y) : src.max_y.Value();

    // Bound to render area
    min_y = (std::max)(min_y, 0);
    max_y = (std::max)(max_y, 0);

    if (src.enable) {
        scissor.offset.x = scale_up(src.min_x);
        scissor.offset.y = scale_up(min_y);
        scissor.extent.width = scale_up(src.max_x - src.min_x);
        scissor.extent.height = scale_up(max_y - min_y);
    } else {
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = (std::numeric_limits<s32>::max)();
        scissor.extent.height = (std::numeric_limits<s32>::max)();
    }
    return scissor;
}

DrawParams MakeDrawParams(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state, u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // 6 triangle vertices per quad, base vertex is part of the index
    // See BindQuadIndexBuffer for more details
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}

bool SupportsPrimitiveRestart(VkPrimitiveTopology topology) {
    switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return false;
    default:
        return true;
    }
}

/// Before a guest dispatch: earlier writes visible to it, its indirect arguments included. `src`
/// is what the writes since the last such barrier may come from. Recorded in the dispatch's own
/// command (as #805 b62f49104d does) instead of one of its own.
void RecordDispatchBarrier(vk::CommandBuffer cmdbuf, VkPipelineStageFlags src) {
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    cmdbuf.PipelineBarrier(src,
                           VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, READ_BARRIER);
}

/// EXPERIMENT (do not merge): EDEN_NX2_EXP_DISPATCH_BARRIERS=all records the barrier before every
/// guest dispatch from all stages, as before hazard tracking (the decisions are still counted).
bool AllDispatchBarriers() {
    return Common::Nx2Switches::Get(Common::Nx2Switches::Switch::DispatchBarriersAll) != 0;
}

bool IsPrimitiveRestartSupported(const Device& device, VkPrimitiveTopology topology) {
    return ((topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsTopologyListPrimitiveRestartSupported()) ||
            SupportsPrimitiveRestart(topology) ||
            (topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsPatchListPrimitiveRestartSupported()));
}

} // Anonymous namespace

void RasterizerVulkan::ReadMeshEmulationProbe() {
    const MeshEmulationProbe probe{mesh_probe};
    mesh_probe.pending = false;
    const size_t num_words{probe.data.size() / sizeof(u32)};
    const auto word = [&](u64 index) -> u32 {
        if (index >= num_words) {
            return 0;
        }
        u32 value;
        std::memcpy(&value, probe.data.data() + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto position = [&](u64 base, u32 vertex) {
        std::array<f32, 4> result{};
        for (u32 element = 0; element < 4; ++element) {
            const u32 line{probe.position_lines[element]};
            if (line != ~0U) {
                result[element] = std::bit_cast<f32>(word(base + u64{vertex} * probe.vertex_lines +
                                                          line));
            }
        }
        return result;
    };
    u64 triangles{}, degenerate{}, outside{}, bad_index{};
    std::string sample;
    for (u32 group = 0; group < probe.groups; ++group) {
        const u64 base{u64{group} * probe.block_words};
        const u64 index_base{base + u64{probe.max_vertices} * probe.vertex_lines};
        for (u32 primitive = 0; primitive < probe.max_primitives; ++primitive) {
            const std::array<u32, 3> indices{word(index_base + primitive * 3),
                                             word(index_base + primitive * 3 + 1),
                                             word(index_base + primitive * 3 + 2)};
            if (indices[0] == indices[1] && indices[1] == indices[2]) {
                ++degenerate;
                continue;
            }
            ++triangles;
            bool any_bad{};
            bool any_inside{};
            for (const u32 vertex : indices) {
                if (vertex >= probe.max_vertices) {
                    any_bad = true;
                    continue;
                }
                const auto p{position(base, vertex)};
                const f32 w{std::abs(p[3])};
                any_inside |= w > 0.0f && std::abs(p[0]) <= w && std::abs(p[1]) <= w;
            }
            bad_index += any_bad;
            outside += !any_inside;
            if (sample.size() < 400 && triangles <= 2) {
                for (const u32 vertex : indices) {
                    const auto p{position(base, std::min(vertex, probe.max_vertices - 1))};
                    sample += fmt::format(" g{} v{} ({:.3g} {:.3g} {:.3g} {:.3g})", group, vertex,
                                          p[0], p[1], p[2], p[3]);
                }
            }
        }
    }
    LOG_INFO(Render_Vulkan,
             "Mesh emulation probe frame {}: {} groups, {} triangles ({} degenerate slots), {} "
             "with every vertex off screen, {} with a bad index; first triangles:{}",
             probe.frame, probe.groups, triangles, degenerate, outside, bad_index, sample);
}

const RasterizerVulkan::MeshArena& RasterizerVulkan::AcquireMeshArena() {
    const u64 tick{scheduler.CurrentTick()};
    if (mesh_arena_current < mesh_arenas.size() && mesh_arenas[mesh_arena_current].tick == tick) {
        return mesh_arenas[mesh_arena_current];
    }
    // A finished submission's counter holds the words its draws asked for, including those
    // that did not fit: grow later arenas to hold them.
    const u64 max_size{(std::min)(u64{1} << 30, u64{device.GetMaxStorageBufferRange()})};
    for (MeshArena& arena : mesh_arenas) {
        if (arena.counted || !scheduler.IsFree(arena.tick)) {
            continue;
        }
        arena.counted = true;
        arena.counter.Invalidate();
        u32 words;
        std::memcpy(&words, arena.counter.Mapped().data(), sizeof(words));
        const u64 needed{u64{words} * 4};
        if (needed > arena.size && mesh_arena_size < max_size) {
            mesh_arena_size = (std::min)(std::bit_ceil(needed + needed / 4), max_size);
            LOG_INFO(Render_Vulkan, "Mesh emulation: a submission needed {} MiB of output, arenas "
                                    "grow to {} MiB",
                     needed >> 20, mesh_arena_size >> 20);
        }
    }
    std::erase_if(mesh_arenas, [&](const MeshArena& arena) {
        return arena.counted && arena.size < mesh_arena_size;
    });
    auto it{std::ranges::find_if(mesh_arenas, [](const MeshArena& arena) { return arena.counted; })};
    if (it == mesh_arenas.end()) {
        const auto make_buffer = [&](u64 size, VkBufferUsageFlags usage, MemoryUsage memory) {
            return memory_allocator.CreateBuffer(
                VkBufferCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = size,
                    .usage = usage,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                },
                memory);
        };
        it = mesh_arenas.insert(
            mesh_arenas.end(),
            MeshArena{
                .buffer = make_buffer(mesh_arena_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      MemoryUsage::DeviceLocal),
                .counter = make_buffer(sizeof(u32), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                       MemoryUsage::Download),
                .size = mesh_arena_size,
            });
    }
    // The GPU is done with this arena, and this submission's commands have not been sent yet.
    const u32 zero{};
    std::memcpy(it->counter.Mapped().data(), &zero, sizeof(zero));
    it->counter.Flush();
    it->tick = tick;
    it->counted = false;
    mesh_arena_current = static_cast<size_t>(std::distance(mesh_arenas.begin(), it));
    return *it;
}

RasterizerVulkan::RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                                   Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                   const Device& device_, MemoryAllocator& memory_allocator_,
                                   StateTracker& state_tracker_, Scheduler& scheduler_)
    : gpu{gpu_}, device_memory{device_memory_}, device{device_},
      memory_allocator{memory_allocator_}, state_tracker{state_tracker_}, scheduler{scheduler_},
      staging_pool(device, memory_allocator, scheduler), descriptor_pool(device, scheduler),
      guest_descriptor_queue(device, UpdateDescriptorQueue::GUEST_FRAME_PAYLOAD_SIZE,
                             device.IsExtDescriptorBufferSupported()),
      compute_pass_descriptor_queue(device, UpdateDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE),
      descriptor_buffer_ring(device, memory_allocator),
      blit_image(device, scheduler, state_tracker, descriptor_pool), render_pass_cache(device),
      texture_cache_runtime{
          device,     scheduler,         memory_allocator, staging_pool,
          blit_image, render_pass_cache, descriptor_pool,  compute_pass_descriptor_queue},
      texture_cache(texture_cache_runtime, device_memory),
      buffer_cache_runtime(device, memory_allocator, scheduler, staging_pool,
                           guest_descriptor_queue, compute_pass_descriptor_queue, descriptor_pool),
      buffer_cache(device_memory, buffer_cache_runtime),
      query_cache_runtime(this, device_memory, buffer_cache, device, memory_allocator, scheduler,
                          staging_pool, compute_pass_descriptor_queue, descriptor_pool, texture_cache),
      query_cache(gpu, *this, device_memory, query_cache_runtime),
      pipeline_cache(device_memory, device, scheduler, descriptor_pool, guest_descriptor_queue,
                     descriptor_buffer_ring, render_pass_cache, buffer_cache, texture_cache,
                     gpu.ShaderNotify()),
      accelerate_dma(buffer_cache, texture_cache, scheduler),
      fence_manager(*this, gpu, texture_cache, buffer_cache, query_cache, device, scheduler),
      frame_dump(device, memory_allocator, scheduler),
      draw_debug_hooks{
          VideoCommon::AnyDrawDebugSwitch([](const char* name) { return std::getenv(name); })} {
    scheduler.SetQueryCache(query_cache);
    buffer_cache.SetPendingCpuWrites(
        [this](const std::function<void(DAddr, size_t)>& callback) {
            gpu.ForEachPendingCpuWrite(callback);
        });
    VideoCommon::GpuSplit::Get().SetIdleHook(
        [](void* argument) { static_cast<Scheduler*>(argument)->FlushDeferred(); },
        &scheduler);
    VideoCommon::GpuSplit::Get().SetFenceQuietCheck(
        [](void* argument, u64* epoch) {
            return static_cast<RasterizerVulkan*>(argument)->FencesQuiet(*epoch);
        },
        this);
    // The draw input watch protects pages draws read and never trusts pages the GPU writes
    // (images, queries, GPU-written buffers). Only Switch 2 sessions mark GPU writes.
    VideoCommon::GetDrawInputWatch().Bind(
        [](void* argument, DAddr address, u64 size, s32 delta) {
            static_cast<RasterizerVulkan*>(argument)->device_memory.UpdatePagesCachedCount(
                address, size, delta);
        },
        [](void* argument, DAddr page) {
            const auto& self = *static_cast<RasterizerVulkan*>(argument);
            constexpr u64 size = Core::DEVICE_PAGESIZE;
            return !Tegra::g_nx2_ampere.load(std::memory_order_relaxed) ||
                   Tegra::Nx2MayBeGpuWritten(page, size) ||
                   self.texture_cache.page_filter.Any(page, size) ||
                   self.query_cache.page_filter.Any(page, size);
        },
        this);
    // Whoever takes or releases the buffer cache's lock first applies the guest write traps
    // queued while it was held, as each trap would have once it got the lock (OnCPUWrite).
    buffer_cache.mutex.SetOwnerHook(
        [](void* argument) {
            auto& self = *static_cast<RasterizerVulkan*>(argument);
            self.write_handoff.Drain([&self](DAddr addr, u64 size) {
                if (!self.buffer_cache.OnCPUWrite(addr, size)) {
                    return;
                }
                // Part of it is GPU-modified: collect those parts, 2 KiB at a time, as the
                // traps would have (OnCPUWrite has no effect when it returns true).
                constexpr u64 page_size = VideoCommon::Nx2WriteHandoff::PageSize;
                for (DAddr page = addr; page < addr + size;) {
                    const DAddr page_end =
                        (std::min)(addr + size, Common::AlignDown(page, page_size) + page_size);
                    if (self.buffer_cache.OnCPUWrite(page, page_end - page)) {
                        self.gpu.CollectHandoffWrite(page, page_end - page);
                    }
                    page = page_end;
                }
            });
        },
        this);
}

RasterizerVulkan::~RasterizerVulkan() {
    VideoCommon::GetDrawInputWatch().Unbind();
    buffer_cache.mutex.SetOwnerHook(nullptr, nullptr);
    VideoCommon::GpuSplit::Get().SetIdleHook(nullptr, nullptr);
    VideoCommon::GpuSplit::Get().SetFenceQuietCheck(nullptr, nullptr);
    scheduler.WaitWorker();
    scheduler.Finish();
}

// Experimental adaptation of PokeDinner commit 76df4591a747: execute the reset shader's
// indirect record writes on the host until general GPU-address translation is available.
bool RasterizerVulkan::TryNx2IndirectReset(bool is_indexed, u32 instance_count) {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_RESETFIX");
        return !value || std::string_view{value} != "0";
    }();
    const auto& regs = maxwell3d->regs;
    const auto& draw = maxwell3d->draw_manager.draw_state;
    if (!enabled || is_indexed || instance_count != 1 || regs.IsMeshEnabled() ||
        draw.topology != Maxwell::PrimitiveTopology::Points ||
        !maxwell3d->GetNativeMacroEngine().native_turing) {
        return false;
    }
    const GPUVAddr program =
        (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
    std::array<u32, VideoCommon::NativeIndirectReset::Program.size()> code{};
    if (!gpu_memory->IsFullyMappedRange(program + 128, sizeof(code))) {
        return false;
    }
    gpu_memory->ReadBlock(program + 128, code.data(), sizeof(code));
    if (!VideoCommon::NativeIndirectReset::Matches(code)) {
        return false;
    }
    FlushWork();
    gpu_memory->FlushCaching();
    const auto reject = [](std::string_view reason) {
        static u32 reported_rejections{};
        if (reported_rejections++ < 8) {
            LOG_WARNING(HW_GPU, "Experimental indirect reset skipped: {}", reason);
        }
        return false;
    };
    const auto in_gpu_range = [this](u64 address, size_t size) {
        return size != 0 && address <= (std::numeric_limits<u64>::max)() - size &&
               gpu_memory->IsWithinGPUAddressRange(address) &&
               gpu_memory->IsWithinGPUAddressRange(address + size - 1);
    };
    const auto& cbuf = maxwell3d->state.shader_stages[0].const_buffers[0];
    if (!cbuf.enabled || cbuf.size < 0x120 || !in_gpu_range(cbuf.address, 0x120) ||
        !gpu_memory->IsFullyMappedRange(cbuf.address + 0x110, 16)) {
        return reject("descriptor unavailable");
    }
    std::array<u64, 2> descriptor{};
    gpu_memory->ReadBlock(cbuf.address + 0x110, descriptor.data(), sizeof(descriptor));
    const u64 list = descriptor[0];
    const u64 first = draw.vertex_buffer.first;
    const u64 count = draw.vertex_buffer.count;
    const auto range = VideoCommon::IndirectResetRange(list, static_cast<u32>(descriptor[1]),
                                                        first, count);
    if (!range || !in_gpu_range(range->begin, range->size) ||
        !gpu_memory->IsFullyMappedRange(range->begin, range->size)) {
        return reject("list outside mapped descriptor");
    }
    std::vector<u64> pointers(count);
    gpu_memory->ReadBlock(range->begin, pointers.data(), range->size);
    static constexpr std::array<u32, 5> record{0, 1, 0, 0, 0};
    for (const u64 pointer : pointers) {
        if (!pointer) {
            return reject("record is null");
        }
        if (!in_gpu_range(pointer, sizeof(record)) ||
            !gpu_memory->IsFullyMappedRange(pointer, sizeof(record))) {
            return reject("record not mapped");
        }
        if (pointer < range->begin + range->size && range->begin < pointer + sizeof(record)) {
            return reject("record overlaps pointer list");
        }
        const auto segments = gpu_memory->GetSubmappedRange(pointer, sizeof(record));
        if (segments.size() != 1 || segments[0].first != pointer ||
            segments[0].second != sizeof(record)) {
            return reject("record crosses a mapping boundary");
        }
    }
    const std::span<const u8> bytes{reinterpret_cast<const u8*>(record.data()), sizeof(record)};
    for (const u64 pointer : pointers) {
        AccelerateInlineToMemory(pointer, bytes.size(), bytes);
    }
    static u32 reported{};
    if (reported++ < 5) {
        LOG_INFO(HW_GPU, "Experimental indirect reset: {} records", count);
    }
    gpu.TickWork();
    return true;
}

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func, bool is_indirect) {

    if (draw_debug_hooks && Common::DkbFrameTrace::Enabled()) {
        ++trace_draws;
    }

    SCOPE_EXIT {
        gpu.TickWork();
    };
    bool submitted = false;
    SCOPE_EXIT {
        auto& native = maxwell3d->GetNativeMacroEngine();
        if (native.native_turing && !submitted) {
            native.native_execution_failed = true;
            const auto& regs = maxwell3d->regs;
            const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) |
                           regs.pipelines[1].reserved[1];
            const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) |
                           regs.pipelines[5].reserved[1];
            LOG_DEBUG(HW_GPU, "Native graphics draw not recorded: frame={} vs={:#x} ps={:#x}",
                      render_trace_frame, vs, ps);
        }
    };
    namespace Profile = VideoCommon::Nx2Profile;
    {
        Profile::Scope scope{Profile::Zone::FlushWork};
        FlushWork();
        gpu_memory->FlushCaching();
    }
    if (frame_dump.Capturing()) {
        frame_dump.DrainIfFull();
    }

    const auto report_failure = [&](std::string_view reason) {
        TraceDrawFailure(reason);
        static std::set<std::tuple<u64, u64, std::string_view>> seen;
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        if (seen.size() < 64 && seen.emplace(vs, ps, reason).second) {
            LOG_WARNING(HW_GPU, "Draw skipped ({}): vs={:#x} ps={:#x} rt0={:#x} {}x{}", reason, vs, ps,
                        regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height);
        }
    };
    GraphicsPipeline* const pipeline{[&] {
        Profile::Scope scope{Profile::Zone::PipelineLookup};
        return pipeline_cache.CurrentGraphicsPipeline();
    }()};
    if (VideoCommon::OotDlss::Requested() &&
        Settings::GetCurrentProgramID() == VideoCommon::OotDlss::ProgramId) [[unlikely]] {
        CaptureOotCamera();
    }
    if (pipeline && VideoCommon::OotDlss::ObjectMotionRequested() &&
        Settings::GetCurrentProgramID() == VideoCommon::OotDlss::ProgramId &&
        maxwell3d->regs.zeta_enable && !pipeline->UsesOotMotion()) [[unlikely]] {
        // A scene draw that falls back to camera motion: named once, with the reason.
        const u64 depth = VideoCommon::OotDlss::Targets().depth_gpu.load(std::memory_order_relaxed);
        if (depth != 0 && maxwell3d->regs.zeta.Address() == depth &&
            oot_reject_seen.insert(pipeline).second) {
            const auto& hashes = pipeline->ShaderHashes();
            u64 geometry = 0;
            for (size_t index = 1; index + 1 < hashes.size() && geometry == 0; ++index) {
                geometry = hashes[index];
            }
            if (const char* reason = VideoCommon::OotDlss::FirstSceneReject(geometry, hashes[5])) {
                static std::atomic<u32> logged{};
                if (logged.fetch_add(1, std::memory_order_relaxed) < 256) {
                    LOG_INFO(Render_Vulkan,
                             "OoT DLSS: scene draws without object motion: geometry {:016x} "
                             "fs {:016x} ({})",
                             geometry, hashes[5], reason);
                }
            }
        }
    }
    if (pipeline && VideoCommon::OotDlss::ReplacesNetwork() &&
        Settings::GetCurrentProgramID() == VideoCommon::OotDlss::ProgramId) [[unlikely]] {
        // A tonemap pass: a known fragment shader, or the tonemap vertex shader drawing a
        // B10G11R11 target of the scene's size.
        const auto& targets = VideoCommon::OotDlss::Targets();
        const auto& rt0 = maxwell3d->regs.rt[0];
        if (oot_post_fog_recording &&
            rt0.Address() == targets.color_gpu.load(std::memory_order_relaxed)) {
            // After the fog composite the game draws more opaque scene (the water surface) and
            // the particles, transparent and not writing depth. DLSS (or the copy for it) runs at
            // the first draw that does not write depth: the opaque draws before it stay in its
            // input, jittered and anti-aliased; the particles after it never enter its history.
            const auto& regs = maxwell3d->regs;
            const auto dst = regs.blend_per_target_enabled != 0
                                 ? regs.blend_per_target[0].color_dest
                                 : regs.blend.color_dest;
            using Factor = Tegra::Engines::Maxwell3D::Regs::Blend::Factor;
            const bool writes_depth = regs.depth_test_enable && regs.depth_write_enabled;
            const bool trigger =
                !writes_depth && oot_dlss_after_fog_frame ==
                                     VideoCommon::OotDlss::SceneFrame().load(std::memory_order_relaxed);
            if (oot_post_fog_order.size() < 400) {
                oot_post_fog_order += trigger ? "|" : "";
                oot_post_fog_order += writes_depth             ? 'O'
                                      : regs.blend.enable[0] == 0 ? 'o'
                                      : dst == Factor::One_D3D || dst == Factor::One_GL ? 'a'
                                                                                        : 'b';
            }
            if (trigger) {
                oot_dlss_after_fog_frame = ~0ULL;
                if (VideoCommon::OotDlss::HdrInput()) {
                    OotDlssBeforeTonemap();
                } else {
                    OotSnapshotScene(0);
                }
            }
        }
        if (VideoCommon::OotDlss::Requested() &&
            VideoCommon::OotDlss::HdrDlssStage() != VideoCommon::OotDlss::HdrStage::Tonemap &&
            rt0.Address() == targets.color_gpu.load(std::memory_order_relaxed)) {
            for (const u64 hash : pipeline->ShaderHashes()) {
                if (!VideoCommon::OotDlss::IsFogCompositeShader(hash)) {
                    continue;
                }
                if (VideoCommon::OotDlss::HdrInput() &&
                    VideoCommon::OotDlss::HdrDlssStage() == VideoCommon::OotDlss::HdrStage::Fog) {
                    OotDlssBeforeTonemap();
                } else {
                    oot_dlss_after_fog_frame =
                        VideoCommon::OotDlss::SceneFrame().load(std::memory_order_relaxed);
                }
                oot_post_fog_order.clear();
                oot_post_fog_recording = true;
                break;
            }
        }
        // The effects layer: one RGBA16F target at half the scene's size (the volumetric fog
        // draws two at once).
        if (VideoCommon::OotDlss::Requested() && VideoCommon::OotDlss::MaskGain() > 0.0f &&
            maxwell3d->regs.rt_control.count == 1 &&
            rt0.format == Tegra::RenderTargetFormat::R16G16B16A16_FLOAT &&
            rt0.width * 2 == targets.depth_width.load(std::memory_order_relaxed) &&
            rt0.height * 2 == targets.depth_height.load(std::memory_order_relaxed)) {
            if (static u32 logged{}; (!oot_effects_rt || oot_effects_rt->Address() != rt0.Address()) &&
                                     logged++ < 4) {
                LOG_INFO(Render_Vulkan, "OoT DLSS: effects layer at {:#x} {}x{}", rt0.Address(),
                         u32{rt0.width}, u32{rt0.height});
            }
            oot_effects_rt = rt0;
        }
        // The volumetric fog: the second of two RGBA16F targets at half the scene's size.
        const auto& rt1 = maxwell3d->regs.rt[1];
        if (VideoCommon::OotDlss::Requested() && VideoCommon::OotDlss::FogMaskGain() > 0.0f &&
            maxwell3d->regs.rt_control.count == 2 &&
            rt1.format == Tegra::RenderTargetFormat::R16G16B16A16_FLOAT &&
            rt1.width * 2 == targets.depth_width.load(std::memory_order_relaxed) &&
            rt1.height * 2 == targets.depth_height.load(std::memory_order_relaxed)) {
            if (static u32 logged{}; (!oot_fog_rt || oot_fog_rt->Address() != rt1.Address()) &&
                                     logged++ < 4) {
                LOG_INFO(Render_Vulkan, "OoT DLSS: fog layer at {:#x}", rt1.Address());
            }
            oot_fog_rt = rt1;
        }
        const bool tonemap_target =
            rt0.format == Tegra::RenderTargetFormat::B10G11R11_FLOAT &&
            rt0.width == targets.depth_width.load(std::memory_order_relaxed) &&
            rt0.height == targets.depth_height.load(std::memory_order_relaxed);
        for (const u64 hash : pipeline->ShaderHashes()) {
            if (VideoCommon::OotDlss::IsTonemapShader(hash) ||
                (tonemap_target && VideoCommon::OotDlss::IsTonemapVertexShader(hash))) {
                const u64 target = maxwell3d->regs.rt[0].Address();
                auto& tonemapped = VideoCommon::OotDlss::Targets().tonemapped_gpu;
                if (target != 0 && tonemapped.exchange(target, std::memory_order_relaxed) != target) {
                    if (static u32 logged{}; logged++ < 8) {
                        LOG_INFO(Render_Vulkan, "OoT DLSS: tonemapped scene at {:#x}", target);
                    }
                }
                // The scene-colour draws after the fog composite, once a while: O opaque
                // (writes depth), o opaque, b blended, a additive; | where DLSS ran.
                if (std::exchange(oot_post_fog_recording, false)) {
                    static u32 frames{}, logged{};
                    if (frames++ % 600 == 0 && logged++ < 8) {
                        LOG_INFO(Render_Vulkan, "OoT DLSS: after the fog composite: {}",
                                 oot_post_fog_order);
                    }
                }
                // Its own shape: the copy and DLSS read it with this, not the output's.
                auto& shape = VideoCommon::OotDlss::Targets();
                shape.tonemapped_width.store(rt0.width, std::memory_order_relaxed);
                shape.tonemapped_height.store(rt0.height, std::memory_order_relaxed);
                shape.tonemapped_layout.store(
                    u32{rt0.tile_mode.block_width} | (u32{rt0.tile_mode.block_height} << 4) |
                        (u32{rt0.tile_mode.block_depth} << 8) |
                        (rt0.tile_mode.is_pitch_linear ? 1u << 12 : 0u) |
                        (rt0.format == Tegra::RenderTargetFormat::B10G11R11_FLOAT ? 0u : 1u << 13),
                    std::memory_order_relaxed);
                if (const auto& cb3 = maxwell3d->state.shader_stages[4].const_buffers[3];
                    cb3.enabled && cb3.size >= 80) {
                    // c[3]: a colour matrix, then (?, exposure, ?, ?) and the curve's peak,
                    // contrast, shoulder and toe. Logged as it changes.
                    std::array<f32, 8> curve{};
                    gpu_memory->ReadBlockUnsafe(cb3.address + 48, curve.data(), sizeof(curve));
                    if (curve[1] > 0.0f && std::isfinite(curve[1]) &&
                        shape.exposure.exchange(curve[1], std::memory_order_relaxed) !=
                            curve[1]) {
                        if (static u32 logged{}; logged++ < 32) {
                            LOG_INFO(Render_Vulkan, "OoT DLSS: tonemap exposure {} (c[3] 48: {})",
                                     curve[1], fmt::join(curve, " "));
                        }
                    }
                }
                if (VideoCommon::OotDlss::HdrInput()) {
                    OotDlssBeforeTonemap();
                } else if (!oot_redrawing &&
                           shape.particle_base_frame.load(std::memory_order_relaxed) ==
                               VideoCommon::OotDlss::SceneFrame().load(std::memory_order_relaxed)) {
                    oot_redraw_tonemap = OotSnapshotScene(1);
                }
                break;
            }
        }
    }
    if (!pipeline) {
        report_failure("no pipeline");
        TraceRenderState(is_indexed, "no-pipeline", nullptr);
        // Mesh emulation survey: how many task/mesh draws a device without
        // VK_EXT_mesh_shader drops, how many are indirect and how many groups they launch.
        if (maxwell3d->regs.IsMeshEnabled() && !device.IsExtMeshShaderSupported()) {
            static u64 draws{}, indirect{}, groups{}, max_groups{};
            ++draws;
            if (is_indirect) {
                ++indirect;
            } else {
                const u64 count{maxwell3d->draw_manager.draw_state.vertex_buffer.count};
                groups += count;
                max_groups = std::max(max_groups, count);
            }
            if (draws == 1 || draws % 5000 == 0) {
                LOG_WARNING(Render_Vulkan,
                            "Mesh emulation survey: {} mesh draws dropped, {} indirect, {} groups "
                            "in direct ones (max {} per draw), frame {}",
                            draws, indirect, groups, max_groups, render_trace_frame);
            }
        }
        return;
    }
    if (VideoCommon::OotNetwork::BypassEnabled(Settings::GetCurrentProgramID())) {
        TrackOotTonemappedImage(*pipeline);
    }
    if (pipeline->UsesOotMotion()) [[unlikely]] {
        PrepareOotObjectMotion();
    }
    // Task/mesh pipelines: the draw's vertex count is the number of task workgroups.
    mesh_draw = pipeline->IsMeshPipeline();
    // EDEN_NX2_NO_MESH=1: skip task/mesh draws as before they were supported (A/B comparisons).
    // EDEN_NX2_NO_MESH_FILE=<path>: skip them while that file exists (checked every 1024 draws),
    // to compare the same scene with and without them in one run.
    static const bool no_mesh_always{std::getenv("EDEN_NX2_NO_MESH") != nullptr};
    static const char* const no_mesh_file{std::getenv("EDEN_NX2_NO_MESH_FILE")};
    static bool no_mesh{no_mesh_always};
    static u32 no_mesh_check{};
    if (no_mesh_file && (no_mesh_check++ % 1024) == 0) {
        no_mesh = std::filesystem::exists(no_mesh_file);
    }
    const bool emulated_mesh{mesh_draw && pipeline->IsEmulatedMesh()};
    if (mesh_draw &&
        (no_mesh || (!device.IsExtMeshShaderSupported() && !emulated_mesh) ||
         (emulated_mesh && is_indirect))) {
        report_failure(no_mesh         ? "EDEN_NX2_NO_MESH"
                       : emulated_mesh ? "indirect emulated mesh draw"
                                       : "no VK_EXT_mesh_shader");
        TraceRenderState(is_indexed, "mesh-skipped", nullptr);
        return;
    }
    // Mesh emulation: one output block per mesh workgroup, written by the pipeline's compute
    // dispatch and drawn as groups * vertices-per-group triangle list vertices.
    mesh_emulation_groups = 0;
    mesh_emulation_vertices_per_group = 0;
    if (emulated_mesh) {
        // The same group count the native path hands vkCmdDrawMeshTasksEXT (MakeDrawParams:
        // the index count for indexed draws), within the device's dispatch limit.
        const auto& draw_state{maxwell3d->draw_manager.draw_state};
        const u32 groups{(std::min)(MakeDrawParams(draw_state, 1, is_indexed).num_vertices,
                                    device.GetMaxComputeWorkGroupCount()[0])};
        // Mesh emulation statistics: draws with and without groups, and the frames with any
        // groups (the game culls on the GPU and sets the counts itself).
        {
            static u64 draws{}, empty{}, total_groups{}, frames{}, frames_drawn{};
            static u64 last_frame{~0ULL};
            static bool frame_drawn{};
            if (render_trace_frame != last_frame) {
                if (last_frame != ~0ULL) {
                    ++frames;
                    frames_drawn += frame_drawn;
                }
                last_frame = render_trace_frame;
                frame_drawn = false;
            }
            ++draws;
            empty += groups == 0;
            total_groups += groups;
            frame_drawn |= groups != 0;
            if (draws % 2000 == 0) {
                LOG_INFO(Render_Vulkan,
                         "Mesh emulation draws: {} ({} with no groups), {} groups; frames with "
                         "emulated draws: {} of {}",
                         draws, empty, total_groups, frames_drawn, frames);
            }
        }
        // Read the pending probe before Request can hand its buffer out again.
        if (mesh_probe.pending && scheduler.IsFree(mesh_probe.tick)) {
            ReadMeshEmulationProbe();
        }
        // EDEN_NX2_MESH_EMULATION_PROBE=1: every 60 frames, read one draw's output back (from a
        // host-visible download buffer, as device-local ones are not mapped on every GPU).
        static const bool probe{std::getenv("EDEN_NX2_MESH_EMULATION_PROBE") != nullptr};
        const bool has_task{pipeline->MeshEmulationHasTask()};
        const bool probe_draw{probe && !has_task && !mesh_probe.pending && groups != 0 &&
                              render_trace_frame % 60 == 0};
        // With a task program the draw's groups are task workgroups. The mesh workgroups they ask
        // for are only known on the GPU, so their output goes to the submission's arena.
        const u64 block_bytes{pipeline->MeshEmulationBlockBytes()};
        StagingBufferRef output{};
        u64 bytes{};
        if (has_task) {
            const MeshArena& arena{AcquireMeshArena()};
            bytes = arena.size;
            pipeline->SetMeshEmulationOutput(*arena.buffer, 0, bytes, groups);
        } else {
            bytes = (std::max)(u64{groups} * block_bytes, u64{4});
            output = staging_pool.Request(bytes, probe_draw ? MemoryUsage::Download
                                                            : MemoryUsage::DeviceLocal);
            pipeline->SetMeshEmulationOutput(output.buffer, output.offset, bytes, groups);
        }
        mesh_emulation_indirect = {};
        if (mesh_task_probe.pending && scheduler.IsFree(mesh_task_probe.tick)) {
            mesh_task_probe.pending = false;
            std::array<u32, Shader::Backend::SPIRV::MESH_EMULATION_ARGS_WORDS> words{};
            std::memcpy(words.data(), mesh_task_probe.args.data(), sizeof(words));
            LOG_INFO(Render_Vulkan,
                     "Mesh emulation task probe: {} tasks asked for {} mesh workgroups, {} "
                     "dispatched at arena word {} (arena {} words)",
                     mesh_task_probe.tasks,
                     words[Shader::Backend::SPIRV::MESH_EMULATION_ARGS_REQUESTED], words[0],
                     words[Shader::Backend::SPIRV::MESH_EMULATION_ARGS_BASE],
                     mesh_task_probe.capacity);
        }
        if (has_task && groups != 0) {
            const bool task_probe{probe && !mesh_task_probe.pending &&
                                  render_trace_frame % 60 == 0};
            const auto request = [&](u64 size, MemoryUsage usage = MemoryUsage::DeviceLocal) {
                size = (std::max)(size, u64{16});
                const StagingBufferRef ref{staging_pool.Request(size, usage)};
                return std::make_pair(GraphicsPipeline::MeshEmulationBuffer{ref.buffer, ref.offset,
                                                                            size},
                                      ref.mapped_span.subspan(
                                          (std::min)(ref.offset, u64{ref.mapped_span.size()})));
            };
            const auto payload{
                request(u64{groups} * pipeline->MeshEmulationPayloadStride() * 4).first};
            const auto offsets{request(u64{groups} * 4).first};
            constexpr u64 args_bytes{Shader::Backend::SPIRV::MESH_EMULATION_ARGS_WORDS * 4};
            const auto [args, args_span] =
                request(args_bytes, task_probe ? MemoryUsage::Download : MemoryUsage::DeviceLocal);
            const u32 capacity_words{static_cast<u32>(
                (std::min)(bytes / 4, u64{(std::numeric_limits<u32>::max)()}))};
            if (task_probe && args_span.size() >= args_bytes) {
                mesh_task_probe = {
                    .args = args_span,
                    .tick = scheduler.CurrentTick(),
                    .tasks = groups,
                    .capacity = capacity_words,
                    .pending = true,
                };
            }
            const MeshArena& arena{mesh_arenas[mesh_arena_current]};
            pipeline->SetMeshEmulationTaskBuffers(
                payload, offsets, args, {*arena.counter, 0, sizeof(u32)}, capacity_words);
            mesh_emulation_indirect = {args.buffer, args.offset};
        }
        if (probe_draw && output.mapped_span.size() >= output.offset + bytes) {
            const MeshEmulationSetup& setup{*pipeline->MeshEmulation()};
            mesh_probe = {
                .data = output.mapped_span.subspan(output.offset, bytes),
                .tick = scheduler.CurrentTick(),
                .frame = render_trace_frame,
                .groups = groups,
                .block_words = setup.block_words,
                .vertex_lines = setup.vertex_lines,
                .max_vertices = setup.max_vertices,
                .max_primitives = setup.max_primitives,
                .position_lines = setup.position_lines,
                .pending = true,
            };
        } else if (probe_draw && output.mapped_span.empty()) {
            static bool warned{};
            if (!warned) {
                warned = true;
                LOG_WARNING(Render_Vulkan, "Mesh emulation probe: output buffer is not mapped");
            }
        }
        mesh_emulation_groups = groups;
        mesh_emulation_vertices_per_group = pipeline->MeshEmulationVerticesPerGroup();
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    const u32 repack_dirty = UpdateVertexRepackLayout(pipeline->HasDynamicVertexInput());
    if (repack_dirty) {
        auto& flags = maxwell3d->dirty.flags;
        flags[VideoCommon::Dirty::VertexBuffers] = true;
        flags[Dirty::VertexInput] = true;
        for (u32 binding = 0; binding < 32; ++binding) {
            if (repack_dirty & (1U << binding)) {
                flags[VideoCommon::Dirty::VertexBuffer0 + binding] = true;
            }
        }
    }
    if (Profile::Scope scope{Profile::Zone::Configure}; !pipeline->Configure(is_indexed)) {
        report_failure("configure failed");
        TraceRenderState(is_indexed, "configure-failed", nullptr);
        return;
    }

    if (draw_debug_hooks) {
        DrawDebugHooksConfigured(pipeline, is_indexed, is_indirect && !mesh_draw);
    }
    if (frame_dump.Capturing()) {
        FrameDumpPass(texture_cache.GetFramebuffer());
        frame_dump.NoteDraw(pipeline->ShaderHashes());
        bool recorded = false;
        if (frame_dump.CapturingSampled()) {
            boost::container::small_vector<FrameDump::Sampled, 16> sampled;
            for (const auto& image : pipeline->SampledImages()) {
                sampled.push_back({image.stage, image.tic, &texture_cache.GetImageView(image.id),
                                   &texture_cache.GetSampler(image.sampler)});
            }
            recorded = frame_dump.NoteSampled(sampled);
        }
        if (frame_dump.CapturingBuffers()) {
            recorded |= FrameDumpBuffers(false, pipeline->ShaderHashes());
        }
        if (recorded) {
            scheduler.RequestRenderpass(texture_cache.GetFramebuffer());
        }
    }

    {
        Profile::Scope scope{Profile::Zone::DynamicState};
        UpdateDynamicStates(pipeline);
    }

    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    {
        Profile::Scope scope{Profile::Zone::DrawRecord};
        scheduler.TraceGraphicsPipeline(pipeline->ShaderHashes());
        trace_draw_hashes = {pipeline->ShaderHashes()[1], pipeline->ShaderHashes()[5]};
        oot_motion_pipeline = pipeline->UsesOotMotion() ? pipeline : nullptr;
        if (oot_motion_pipeline && is_indirect) [[unlikely]] {
            PushOotObjectMotion(0, 0, 0, 0, false, false);
        }
        draw_func();
        oot_motion_pipeline = nullptr;
    }
    submitted = true;
    if (VideoCommon::DrawClass::Enabled()) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        VideoCommon::DrawClass::GetStats().NoteDraw({
            .pipeline = pipeline,
            .handle_hash = 0,
            .binding_hash = 0,
            .render_targets_serial = texture_cache.RenderTargetsSerial(),
            .frame = render_trace_frame,
            .first = is_indexed ? draw_state.index_buffer.first : draw_state.vertex_buffer.first,
            .count = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
            .instances = draw_state.instance_count,
            .changes = maxwell3d->TakeDrawChanges(),
            .indexed = is_indexed,
            .bindings_valid = false,
        });
    }
    static u32 native_capture_count{};
    if (maxwell3d->GetNativeMacroEngine().native_turing &&
        native_capture_count < 16 && [] {
            static const bool capture = std::getenv("EDEN_NATIVE_DRAW_CAPTURE") != nullptr;
            return capture;
        }()) {
        const u32 capture = native_capture_count++;
        const auto prefix = fmt::format("native-draw-{}", capture);
        const auto dump = [&](const std::string& name, GPUVAddr address, size_t size) {
            if (!size || !gpu_memory->IsFullyMappedRange(address, size)) return;
            std::vector<u8> bytes(size);
            gpu_memory->ReadBlockUnsafe(address, bytes.data(), bytes.size());
            std::ofstream file(prefix + name + ".bin", std::ios::binary);
            file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            LOG_INFO(HW_GPU, "Native draw capture {}{}: address={:#x}, bytes={}, nonzero={}",
                     capture, name, address, size,
                     std::count_if(bytes.begin(), bytes.end(), [](u8 byte) { return byte != 0; }));
        };
        const auto& regs = maxwell3d->regs;
        const auto& rt = regs.rt[0];
        const size_t size = static_cast<size_t>(rt.array_pitch) * 4;
        if (size && size <= 16 * 1024 * 1024) {
            if (const auto address = gpu_memory->GpuToCpuAddress(rt.Address(), size)) {
                LOG_INFO(HW_GPU, "Native draw {} render target physical address={:#x}", capture, *address);
                texture_cache.DownloadMemory(*address, size);
                dump("-rt0", rt.Address(), size);
            }
        }
        std::ofstream registers(prefix + "-registers.bin", std::ios::binary);
        registers.write(reinterpret_cast<const char*>(&regs), sizeof(regs));
        for (size_t stream = 0; stream < 4; ++stream) {
            if (regs.vertex_streams[stream].IsEnabled())
                dump(fmt::format("-vertex-{}", stream), regs.vertex_streams[stream].Address(), 128);
        }
        for (size_t stage : {0U, 4U}) {
            for (size_t bank : {2U, 3U}) {
                const auto& cb = maxwell3d->state.shader_stages[stage].const_buffers[bank];
                if (cb.enabled) dump(fmt::format("-cbuf-{}-{}", stage, bank), cb.address, std::min<size_t>(cb.size, 128));
            }
        }
    }
}


namespace {
// EDEN_NX2_WINDOW_TRACE: describe clears and draws into screen-sized colour targets (debug aid).
void TraceWindowTarget(const Tegra::Engines::Maxwell3D& maxwell3d, Tegra::MemoryManager& memory,
                       const char* kind, u32 count) {
    static const bool trace = std::getenv("EDEN_NX2_WINDOW_TRACE") != nullptr;
    static u32 traced{};
    const auto& regs = maxwell3d.regs;
    static const u64 skip = [] {
        const char* value = std::getenv("EDEN_NX2_WINDOW_TRACE_SKIP");
        return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ULL;
    }();
    static u64 seen_events{};
    static const u64 only_ps = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_PS");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    const u64 pixel_shader =
        (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
    if (!trace || regs.rt[0].width != 3840 || traced >= 400 || seen_events++ < skip ||
        (only_ps != 0 && pixel_shader != only_ps)) {
        return;
    }
    ++traced;
    const auto& vp = regs.viewport_transform[0];
    const auto& sc = regs.scissor_test[0];
    LOG_INFO(HW_GPU,
             "nx2 window {} rt0={:#x} count={} clear={:#x} color=({},{},{},{}) mask_common={} "
             "mask0={:#x} rt_count={} vp scale=({},{}) translate=({},{}) scissor en={} x={}..{} "
             "y={}..{} blend0={} vs={:#x} ps={:#x} rasterize={}",
             kind, regs.rt[0].Address(), count, regs.clear_surface.raw, regs.clear_color[0],
             regs.clear_color[1], regs.clear_color[2], regs.clear_color[3], regs.color_mask_common,
             regs.color_mask[0].raw, regs.rt_control.count.Value(), vp.scale_x, vp.scale_y,
             vp.translate_x, vp.translate_y, sc.enable, sc.min_x.Value(), sc.max_x.Value(),
             sc.min_y.Value(), sc.max_y.Value(), regs.blend.enable[0],
             (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1],
             (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1],
             regs.rasterize_enable);
    if (kind[0] != 'd') {
        return;
    }
    // Pixel-stage constant buffer 3: colour constants of the loading-screen passes.
    if (const auto& cb3 = maxwell3d.state.shader_stages[4].const_buffers[3]; cb3.enabled) {
        std::array<f32, 8> values{};
        memory.ReadBlockUnsafe(cb3.address, values.data(), sizeof(values));
        LOG_INFO(HW_GPU, "nx2 window ps c[3]@{:#x} size={}: {}", cb3.address, cb3.size,
                 fmt::join(values, " "));
    }
    static u32 texture_traces{};
    if (texture_traces++ > 200) {
        return;
    }
    // Texture used by the pixel shader's first bound TEX (handle in c[2][0x20]) and its TIC.
    const auto& cb = maxwell3d.state.shader_stages[4].const_buffers[2];
    const u32 handle = cb.enabled ? memory.Read<u32>(cb.address + 0x20) : 0;
    const u32 tic_index = handle & 0xfffff;
    const GPUVAddr tic_address = regs.tex_header.Address() + u64{tic_index} * 32;
    std::array<u32, 8> tic{};
    memory.ReadBlockUnsafe(tic_address, tic.data(), sizeof(tic));
    LOG_INFO(HW_GPU,
             "nx2 window texture: cb2={:#x} en={} handle={:#x} tic pool={:#x} limit={} "
             "sampler pool={:#x} tic[{}]={:08x}",
             cb.address, cb.enabled, handle, regs.tex_header.Address(), regs.tex_header.limit,
             regs.tex_sampler.Address(), tic_index, fmt::join(tic, " "));
    // First bytes of the texture's memory (block-linear, so just a sample).
    const GPUVAddr texture = (GPUVAddr{tic[2] & 0xffff} << 32) | tic[1];
    std::array<u32, 16> texels{};
    memory.ReadBlockUnsafe(texture, texels.data(), sizeof(texels));
    LOG_INFO(HW_GPU, "nx2 window texture {:#x} {}x{}: {:08x}", texture, (tic[4] & 0xffff) + 1,
             (tic[5] & 0xffff) + 1, fmt::join(texels, " "));
    // EDEN_NX2_TEXTURE_DUMP=<file>: save the first 4 MiB of the last traced texture's memory.
    if (const char* path = std::getenv("EDEN_NX2_TEXTURE_DUMP"); path != nullptr) {
        std::vector<u8> bytes(2048u * 10240u);
        memory.ReadBlockUnsafe(texture, bytes.data(), bytes.size());
        if (std::FILE* file = std::fopen(path, "wb")) {
            std::fwrite(bytes.data(), 1, bytes.size(), file);
            std::fclose(file);
        }
    }
}
} // Anonymous namespace

// The per-draw debug switches that look at the configured pipeline (DrawDebugSwitches).
YUZU_NO_INLINE void RasterizerVulkan::DrawDebugHooksConfigured(GraphicsPipeline* pipeline,
                                                               bool is_indexed,
                                                               bool plain_indirect) {
    TracePendingCpuWrites(false, pipeline->ShaderHashes());
    TraceRenderState(is_indexed, "configured", texture_cache.GetFramebuffer());
    if (VideoCommon::RenderTrace::SampleFrame(render_trace_frame) && render_trace_draws <= 2048) {
        LOG_DEBUG(Render_Vulkan,
                  "Render trace pipeline: frame={} draw={} hashes={:016x} dynamic_vertex_input={}",
                  render_trace_frame, render_trace_draws, fmt::join(pipeline->ShaderHashes(), ","),
                  pipeline->HasDynamicVertexInput());
    }

    if (plain_indirect && Common::DkbFrameTrace::Enabled()) {
        static const bool readback = [] {
            const char* value = std::getenv("EDEN_DKB_INDIRECT_READBACK");
            return value && std::string_view{value} == "1";
        }();
        if (readback) {
            CaptureIndirectTrace();
        }
    }
}

// The per-draw debug switches (DrawDebugSwitches), out of the draw path. False: skip the draw.
YUZU_NO_INLINE bool RasterizerVulkan::DrawDebugHooks(bool is_indexed) {
    TraceWindowTarget(*maxwell3d, *gpu_memory, is_indexed ? "draw-indexed" : "draw",
                      is_indexed ? maxwell3d->draw_manager.draw_state.index_buffer.count
                                 : maxwell3d->draw_manager.draw_state.vertex_buffer.count);
    // EDEN_NX2_CB24_TRACE: for draws whose vertex shader reads bank 24, show the inline
    // constant buffer upload target (CB_ADDRESS) and its first words.
    if (static const bool trace = std::getenv("EDEN_NX2_CB24_TRACE") != nullptr; trace) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        static u32 logged{};
        if ((vs == 0x11471c9a00 || vs == 0x11471ca600 || vs == 0x11471cd300) && logged++ < 3) {
            std::array<u32, 24> words{};
            gpu_memory->ReadBlockUnsafe(regs.const_buffer.Address(), words.data(), sizeof(words));
            LOG_INFO(HW_GPU, "bank24 draw vs={:#x}: CB_ADDRESS {:#x} size {:#x} offset {:#x}: {:08x}", vs,
                     regs.const_buffer.Address(), regs.const_buffer.size, regs.const_buffer.offset,
                     fmt::join(words, " "));
            // Search every bound vertex-stage buffer for render-target-size values.
            const auto& stage = maxwell3d->state.shader_stages[0];
            for (u32 slot = 0; slot < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& buffer = stage.const_buffers[slot];
                if (!buffer.enabled || buffer.size == 0) continue;
                std::vector<u32> data(std::min<u32>(buffer.size, 0x10000) / 4);
                gpu_memory->ReadBlockUnsafe(buffer.address, data.data(), data.size() * 4);
                for (size_t i = 0; i < data.size(); ++i) {
                    const u32 w = data[i];
                    if (w == 0x45200000 || w == 0x44b40000 || w == 0x39cccccd || w == 0x3a360b61 ||
                        w == 0x44a00000 || w == 0x44340000 || w == 2559 || w == 1439) {
                        LOG_INFO(HW_GPU, "  slot {} +{:#x} = {:#x}", slot, i * 4, w);
                    }
                }
            }
        }
    }
    // EDEN_NX2_TRACE_VS=<address>: describe the next few draws using that vertex shader like the
    // after-upscale trace does (render target, blend, fragment textures).
    static const u64 trace_vs = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_VS");
        return value ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    if (trace_vs != 0) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        static u32 traced{};
        if (vs == trace_vs && traced++ % 200 == 0 && traced < 1000) {
            Tegra::Engines::g_nx2_trace_draws_after_upscale = 1;
        }
    }
    if (Tegra::Engines::g_nx2_trace_draws_after_upscale > 0) {
        --Tegra::Engines::g_nx2_trace_draws_after_upscale;
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        LOG_INFO(HW_GPU, "after upscale: draw rt0 {:#x} {}x{} fmt {:#x} block {}/{}/{} pitch_linear {} depth {} array_pitch {:#x} ctrl {:#x} vs {:#x} ps {:#x}",
                 regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height,
                 static_cast<u32>(regs.rt[0].format), regs.rt[0].tile_mode.block_width.Value(),
                 regs.rt[0].tile_mode.block_height.Value(), regs.rt[0].tile_mode.block_depth.Value(),
                 regs.rt[0].tile_mode.is_pitch_linear.Value(), regs.rt[0].depth.Value(),
                 regs.rt[0].array_pitch, regs.rt_control.count.Value(), vs, ps);
        // Fragment-stage texture handles (bound buffer 2) and the images they name.
        const auto& handles = maxwell3d->state.shader_stages[4].const_buffers[2];
        if (handles.enabled) {
            for (u32 i = 0; i < 24; ++i) {
                const u32 handle = gpu_memory->Read<u32>(handles.address + i * 4);
                if (handle == 0 || handle == 0xffffffff) continue;
                Tegra::Texture::TICEntry tic{};
                gpu_memory->ReadBlockUnsafe(regs.tex_header.Address() + u64{handle & 0xfffff} * sizeof(tic),
                                            &tic, sizeof(tic));
                LOG_INFO(HW_GPU, "  fs handle[{}] = {:#x}: {:#x} {}x{} format {:#x}", i, handle,
                         tic.Address(), tic.Width(), tic.Height(), static_cast<u32>(tic.format.Value()));
            }
        }
        const auto& bt = regs.blend_per_target[0];
        LOG_INFO(HW_GPU, "  blend enable {} per-target {} global op {} src {:#x} dst {:#x} | rt0 op {} src {:#x} dst {:#x} alpha {:#x}/{:#x} mask {:#x}",
                 regs.blend.enable[0], regs.blend_per_target_enabled,
                 static_cast<u32>(regs.blend.color_op), static_cast<u32>(regs.blend.color_source),
                 static_cast<u32>(regs.blend.color_dest), static_cast<u32>(bt.color_op),
                 static_cast<u32>(bt.color_source), static_cast<u32>(bt.color_dest),
                 static_cast<u32>(bt.alpha_source), static_cast<u32>(bt.alpha_dest),
                 regs.color_mask[0].raw);
    }
    // EDEN_NX2_SKIP_VS=<address>[,<address>...]: drop draws using those vertex shaders (debug aid).
    static const std::vector<u64> skip_list = [] {
        std::vector<u64> list;
        if (const char* value = std::getenv("EDEN_NX2_SKIP_VS")) {
            for (char* end = nullptr; *value; value = *end ? end + 1 : end) {
                list.push_back(std::strtoull(value, &end, 16));
                if (end == value) break;
            }
        }
        return list;
    }();
    if (!skip_list.empty()) {
        const auto& regs = maxwell3d->regs;
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        if (std::find(skip_list.begin(), skip_list.end(), vs) != skip_list.end()) {
            return false;
        }
    }
    // EDEN_NX2_RT_TRACE: log the first draw to every distinct colour target (debug aid).
    if (static const bool trace = std::getenv("EDEN_NX2_RT_TRACE") != nullptr; trace) {
        static std::set<std::tuple<GPUVAddr, u32, u32, u64, u64>> seen;
        const auto& regs = maxwell3d->regs;
        const auto& rt = regs.rt[0];
        const u64 vs = (u64{regs.pipelines[1].reserved[0] & 0xff} << 32) | regs.pipelines[1].reserved[1];
        const u64 ps = (u64{regs.pipelines[5].reserved[0] & 0xff} << 32) | regs.pipelines[5].reserved[1];
        const auto key = std::make_tuple(rt.Address(), rt.width, static_cast<u32>(rt.format), vs, ps);
        if (rt.width >= 640 && seen.size() < 600 && seen.insert(key).second) {
            LOG_INFO(HW_GPU, "nx2 draw target {:#x} {}x{} format={:#x} vs={:#x} ps={:#x} blend={} (pairs so far {})",
                     rt.Address(), rt.width, rt.height, static_cast<u32>(rt.format), vs, ps,
                     regs.blend.enable[0], seen.size());
        }
    }
    return true;
}

void RasterizerVulkan::Draw(bool is_indexed, u32 instance_count) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    {
        // A draw's first and count can come from memory a skipped (rejected) compute program
        // should have written. Past 16M vertices they are garbage: building the quad index
        // buffer for one threw out of the GPU thread and ended the process (Hyrule Warriors:
        // Age of Imprisonment, entering a new area), and indexed ones read far out of bounds.
        const auto& state = maxwell3d->draw_manager.draw_state;
        const u64 first = is_indexed ? state.index_buffer.first : state.vertex_buffer.first;
        const u64 count = is_indexed ? state.index_buffer.count : state.vertex_buffer.count;
        constexpr u64 MaxVertices = u64{1} << 24;
        if (first + count > MaxVertices) {
            static std::atomic<u32> skipped{};
            if (skipped.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_WARNING(HW_GPU, "Draw skipped: {} {} from {} (vertex/index count past {})",
                            count, is_indexed ? "indices" : "vertices", first, MaxVertices);
            }
            return;
        }
    }
    if (TryNx2IndirectReset(is_indexed, instance_count)) {
        return;
    }
    if (draw_debug_hooks && !DrawDebugHooks(is_indexed)) {
        return;
    }
    oot_redraw_tonemap = false;
    PrepareDraw(is_indexed, [this, is_indexed, instance_count] {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        const u32 num_instances{instance_count};
        const DrawParams draw_params{MakeDrawParams(draw_state, num_instances, is_indexed)};
        if (VideoCommon::RenderTrace::SampleFrame(render_trace_frame) && render_trace_draws <= 2048) {
            LOG_DEBUG(Render_Vulkan,
                      "Render trace direct: frame={} draw={} count={} instances={} first_index={} "
                      "base_vertex={} base_instance={} mesh={}",
                      render_trace_frame, render_trace_draws, draw_params.num_vertices,
                      draw_params.num_instances, draw_params.first_index, draw_params.base_vertex,
                      draw_params.base_instance, mesh_draw);
        }

        if (Common::DkbFrameTrace::Enabled() &&
            (draw_params.num_vertices == 0 || (!mesh_draw && draw_params.num_instances == 0))) {
            ++trace_zero_direct;
        }

        if (mesh_draw && mesh_emulation_vertices_per_group != 0) {
            const u32 vertices{mesh_emulation_groups * mesh_emulation_vertices_per_group};
            if (vertices == 0) {
                return;
            }
            if (mesh_emulation_indirect.buffer) {
                // With a task program the distribution pass wrote the vertex count.
                scheduler.Record([this, indirect = mesh_emulation_indirect](vk::CommandBuffer cmdbuf) {
                    if (scheduler.GraphicsPipelineMissing()) {
                        return;
                    }
                    cmdbuf.DrawIndirect(indirect.buffer,
                                        indirect.offset +
                                            Shader::Backend::SPIRV::MESH_EMULATION_DRAW_ARGS_OFFSET,
                                        1, 0);
                });
                return;
            }
            scheduler.Record([this, vertices](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                cmdbuf.Draw(vertices, 1, 0, 0);
            });
            return;
        }
        if (mesh_draw) {
            if (oot_motion_pipeline) [[unlikely]] {
                PushOotObjectMotion(draw_params.base_vertex, draw_params.num_vertices, 1, 0, false,
                                    true);
            }
            scheduler.Record([this, groups = draw_params.num_vertices,
                              hashes = trace_draw_hashes](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                scheduler.TraceGpuWork(cmdbuf, hashes.first, hashes.second, {groups, 1, 1},
                                       [&] { cmdbuf.DrawMeshTasksEXT(groups, 1, 1); });
            });
            return;
        }
        if (oot_motion_pipeline) [[unlikely]] {
            PushOotObjectMotion(draw_params.base_vertex, draw_params.num_vertices,
                                draw_params.num_instances, draw_params.base_instance,
                                draw_params.is_indexed, true);
        }
        scheduler.Record([this, draw_params, hashes = trace_draw_hashes](vk::CommandBuffer cmdbuf) {
            if (scheduler.GraphicsPipelineMissing()) {
                return;
            }
            scheduler.TraceGpuWork(
                cmdbuf, hashes.first, hashes.second,
                {draw_params.num_vertices, draw_params.num_instances, 0}, [&] {
                    if (draw_params.is_indexed) {
                        cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                                           draw_params.first_index, draw_params.base_vertex,
                                           draw_params.base_instance);
                    } else {
                        cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                                    draw_params.base_vertex, draw_params.base_instance);
                    }
                });
        });

        // Log draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string params = is_indexed ?
                fmt::format("vertices={}, instances={}, firstIndex={}, baseVertex={}, baseInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.first_index, draw_params.base_vertex, draw_params.base_instance) :
                fmt::format("vertices={}, instances={}, firstVertex={}, firstInstance={}",
                    draw_params.num_vertices, draw_params.num_instances,
                    draw_params.base_vertex, draw_params.base_instance);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                is_indexed ? "vkCmdDrawIndexed" : "vkCmdDraw", params, VK_SUCCESS);
        }
    });
    if (std::exchange(oot_redraw_tonemap, false)) [[unlikely]] {
        OotRedrawTonemap(is_indexed, instance_count);
    }
}

void RasterizerVulkan::DrawIndirect() {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Draw};
    const auto& params = maxwell3d->draw_manager.indirect_state;
    if (Common::DkbFrameTrace::Enabled()) {
        ++trace_indirect;
    }
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, [this, &params] {
        if (VideoCommon::RenderTrace::SampleFrame(render_trace_frame) && render_trace_draws <= 2048) {
            LOG_DEBUG(Render_Vulkan,
                      "Render trace indirect: frame={} draw={} address={:#x} count_address={:#x} "
                      "bytes={} max_draws={} stride={} include_count={} byte_count={} mesh={}",
                      render_trace_frame, render_trace_draws, params.indirect_start_address,
                      params.count_start_address, params.buffer_size, params.max_draw_counts,
                      params.stride, params.include_count, params.is_byte_count, mesh_draw);
        }
        if (mesh_draw) {
            TraceDrawFailure("indirect task/mesh unsupported");
            // Indirect task/mesh draws would need vkCmdDrawMeshTasksIndirectEXT's layout.
            static bool logged{};
            if (!std::exchange(logged, true)) {
                LOG_WARNING(Render_Vulkan, "Indirect task/mesh draw skipped");
            }
            return;
        }
        const auto indirect_buffer = buffer_cache.GetDrawIndirectBuffer();
        const auto& buffer = indirect_buffer.first;
        const auto& offset = indirect_buffer.second;
        if (params.is_byte_count) {
            scheduler.Record([this, buffer_obj = buffer->Handle(), offset,
                              stride = params.stride](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                cmdbuf.DrawIndirectByteCountEXT(1, 0, buffer_obj, offset, 0,
                                                static_cast<u32>(stride));
            });
            return;
        }
        if (params.include_count) {
            const auto count = buffer_cache.GetDrawIndirectCount();
            const auto& draw_buffer = count.first;
            const auto& offset_base = count.second;
            scheduler.Record([this, draw_buffer_obj = draw_buffer->Handle(),
                              buffer_obj = buffer->Handle(), offset_base, offset,
                              params](vk::CommandBuffer cmdbuf) {
                if (scheduler.GraphicsPipelineMissing()) {
                    return;
                }
                if (params.is_indexed) {
                    cmdbuf.DrawIndexedIndirectCount(
                        buffer_obj, offset, draw_buffer_obj, offset_base,
                        static_cast<u32>(params.max_draw_counts), static_cast<u32>(params.stride));
                } else {
                    cmdbuf.DrawIndirectCount(buffer_obj, offset, draw_buffer_obj, offset_base,
                                             static_cast<u32>(params.max_draw_counts),
                                             static_cast<u32>(params.stride));
                }
            });
            return;
        }
        scheduler.Record([this, buffer_obj = buffer->Handle(), offset, params,
                          hashes = trace_draw_hashes](vk::CommandBuffer cmdbuf) {
            if (scheduler.GraphicsPipelineMissing()) {
                return;
            }
            scheduler.TraceGpuWork(cmdbuf, hashes.first, hashes.second, {}, [&] {
                if (params.is_indexed) {
                    cmdbuf.DrawIndexedIndirect(buffer_obj, offset,
                                               static_cast<u32>(params.max_draw_counts),
                                               static_cast<u32>(params.stride));
                } else {
                    cmdbuf.DrawIndirect(buffer_obj, offset,
                                        static_cast<u32>(params.max_draw_counts),
                                        static_cast<u32>(params.stride));
                }
            });
        });

        // Log indirect draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string log_params = fmt::format("drawCount={}, stride={}",
                params.max_draw_counts, params.stride);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                params.is_indexed ? "vkCmdDrawIndexedIndirect" : "vkCmdDrawIndirect",
                log_params, VK_SUCCESS);
        }
    }, true);
    buffer_cache.SetDrawIndirect(nullptr);
}

VideoCommon::OotNetwork::BypassState* RasterizerVulkan::OotNetworkBypass() {
    // NVIDIA DLSS / DLAA replaces the same output pass (and copies the tonemapped image itself
    // on a frame it cannot evaluate).
    if (VideoCommon::OotDlss::Requested()) {
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            LOG_INFO(Render_Vulkan, "Ocarina of Time: No sharpening is off while NVIDIA DLSS is on");
        }
        return nullptr;
    }
    return &oot_network_bypass;
}

void RasterizerVulkan::TrackOotTonemappedImage(const GraphicsPipeline& pipeline) {
    const auto& hashes = pipeline.ShaderHashes();
    if (std::none_of(hashes.begin(), hashes.end(), VideoCommon::OotNetwork::IsTonemapShader)) {
        return;
    }
    const auto& target = maxwell3d->regs.rt[0];
    if (target.Address() == 0) {
        return;
    }
    std::scoped_lock lock{oot_network_bypass.mutex};
    if (oot_network_bypass.tonemapped.Address() != target.Address()) {
        LOG_INFO(Render_Vulkan, "Ocarina of Time: tonemapped image at {:#x} {}x{}", target.Address(),
                 target.width, target.height);
    }
    oot_network_bypass.tonemapped = target;
}

void RasterizerVulkan::TraceDrawFailure(std::string_view reason) {
    if (!Common::DkbFrameTrace::Enabled()) {
        return;
    }
    ++trace_skipped;
    if (trace_failure_details++ >= 4) {
        return;
    }
    const auto& regs = maxwell3d->regs;
    std::array<GPUVAddr, 6> stages{};
    for (size_t i = 0; i < stages.size(); ++i) {
        if (regs.IsShaderConfigEnabled(i)) {
            stages[i] = maxwell3d->GetNativeMacroEngine().native_turing
                            ? (u64{regs.pipelines[i].reserved[0] & 0xff} << 32) |
                                  regs.pipelines[i].reserved[1]
                            : regs.program_region.Address() + regs.pipelines[i].offset;
        }
    }
    LOG_INFO(Render_Vulkan,
             "DKB draw skipped frame={} reason={} stages[0..5]={:#x} rt0={:#x} {}x{}",
             trace_frame, reason, fmt::join(stages, ","), regs.rt[0].Address(),
             regs.rt[0].width, regs.rt[0].height);
}

void RasterizerVulkan::CaptureIndirectTrace() {
    if (!trace_sample_frame || trace_captures >= 4) {
        return;
    }
    ++trace_captures;
    const auto slot = std::find_if(indirect_traces.begin(), indirect_traces.end(),
                                   [](const auto& trace) { return !trace.pending; });
    if (slot == indirect_traces.end()) {
        ++trace_capture_dropped;
        return;
    }
    const auto& params = maxwell3d->draw_manager.indirect_state;
    const auto [commands, command_offset] = buffer_cache.GetDrawIndirectBuffer();
    const u32 max_draws = static_cast<u32>(params.max_draw_counts);
    std::array<VkBufferCopy, 8> copies{};
    u32 sampled{};
    const u64 command_bytes = params.is_byte_count ? 4 : 8;
    const u64 available = command_offset <= commands->SizeBytes()
                              ? std::min<u64>(params.buffer_size,
                                              commands->SizeBytes() - command_offset)
                              : 0;
    const u32 limit = params.is_byte_count ? 1 : std::min(max_draws, 8U);
    for (u32 i = 0; i < limit; ++i) {
        if (available < command_bytes || (i && params.stride > (available - command_bytes) / i)) {
            break;
        }
        const u64 offset = command_offset + i * params.stride;
        copies[sampled++] = VkBufferCopy{offset, i * 8ULL, command_bytes};
    }
    VkBuffer count_buffer{};
    VkBufferCopy count_copy{};
    if (params.include_count && !params.is_byte_count) {
        const auto [count, offset] = buffer_cache.GetDrawIndirectCount();
        if (offset > count->SizeBytes() || sizeof(u32) > count->SizeBytes() - offset) {
            ++trace_capture_dropped;
            return;
        }
        count_buffer = count->Handle();
        count_copy = VkBufferCopy{offset, 64, sizeof(u32)};
    }
    if (!sampled && !count_buffer) {
        LOG_INFO(Render_Vulkan, "DKB indirect frame={} address={:#x} max_draws={} sampled=0",
                 trace_frame, params.indirect_start_address, max_draws);
        return;
    }
    if (!slot->readback) {
        slot->readback = memory_allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = 68,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            },
            MemoryUsage::Download);
    }
    slot->frame = trace_frame;
    slot->address = params.indirect_start_address;
    slot->count_address = params.count_start_address;
    slot->max_draws = max_draws;
    slot->sampled_commands = sampled;
    slot->include_count = count_buffer != VK_NULL_HANDLE;
    slot->byte_count = params.is_byte_count;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([source = commands->Handle(), destination = *slot->readback, copies, sampled,
                      count_buffer, count_copy](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, before);
        if (sampled) {
            cmdbuf.CopyBuffer(source, destination, vk::Span(copies.data(), sampled));
        }
        if (count_buffer) {
            cmdbuf.CopyBuffer(count_buffer, destination, count_copy);
        }
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                               0, after);
    });
    slot->tick = scheduler.CurrentTick();
    slot->pending = true;
    scheduler.RequestRenderpass(texture_cache.GetFramebuffer());
    scheduler.NoteRenderPassStores();
}

void RasterizerVulkan::CollectIndirectTrace() {
    for (auto& trace : indirect_traces) {
        if (!trace.pending || !scheduler.IsFree(trace.tick)) {
            continue;
        }
        trace.readback.Invalidate();
        std::array<u32, 17> words{};
        std::memcpy(words.data(), trace.readback.Mapped().data(), sizeof(words));
        if (trace.byte_count) {
            LOG_INFO(Render_Vulkan, "DKB indirect frame={} address={:#x} gpu_byte_count={}",
                     trace.frame, trace.address, words[0]);
        } else {
            const u32 count = trace.include_count ? words[16] : trace.max_draws;
            const u32 requested_draws = std::min(count, trace.max_draws);
            const u32 sampled = std::min(requested_draws, trace.sampled_commands);
            u32 zero_commands{};
            std::string commands;
            for (u32 i = 0; i < sampled; ++i) {
                zero_commands += words[2 * i] == 0 || words[2 * i + 1] == 0;
                commands += fmt::format("{}{}x{}", i ? "," : "", words[2 * i], words[2 * i + 1]);
            }
            LOG_INFO(Render_Vulkan,
                     "DKB indirect frame={} address={:#x} count_address={:#x} draw_count={} "
                     "count_source={} max_draws={} requested_draws={} sampled={} zero_commands={} "
                     "vertices_or_indices_x_instances=[{}] conditional_execution=unobserved",
                     trace.frame, trace.address, trace.count_address, count,
                     trace.include_count ? "gpu" : "command-parameter",
                     trace.max_draws, requested_draws, sampled, zero_commands, commands);
        }
        trace.pending = false;
    }
}

void RasterizerVulkan::DrawTexture() {

    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();

    std::scoped_lock l{texture_cache.mutex};
    texture_cache.SynchronizeDescriptors(false);
    texture_cache.UpdateRenderTargets(false);

    UpdateDynamicStates(nullptr);

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    const auto& draw_texture_state = maxwell3d->draw_manager.draw_texture_state;
    const auto& sampler = texture_cache.GetSampler(draw_texture_state.src_sampler, false);
    const auto& texture = texture_cache.GetImageView(draw_texture_state.src_texture);
    const auto* framebuffer = texture_cache.GetFramebuffer();
    if (frame_dump.Capturing()) {
        FrameDumpPass(framebuffer);
    }

    const bool src_rescaling = texture_cache.IsRescaling() && texture.IsRescaled();
    const bool dst_rescaling = texture_cache.IsRescaling() && framebuffer->IsRescaled();

    const auto ScaleSrc = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return src_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    const auto ScaleDst = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return dst_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    Region2D dst_region = {Offset2D{.x = ScaleDst(draw_texture_state.dst_x0),
                                    .y = ScaleDst(draw_texture_state.dst_y0)},
                           Offset2D{.x = ScaleDst(draw_texture_state.dst_x1),
                                    .y = ScaleDst(draw_texture_state.dst_y1)}};
    Region2D src_region = {Offset2D{.x = ScaleSrc(draw_texture_state.src_x0),
                                    .y = ScaleSrc(draw_texture_state.src_y0)},
                           Offset2D{.x = ScaleSrc(draw_texture_state.src_x1),
                                    .y = ScaleSrc(draw_texture_state.src_y1)}};
    Extent3D src_size = {static_cast<u32>(ScaleSrc(texture.size.width)),
                         static_cast<u32>(ScaleSrc(texture.size.height)), texture.size.depth};
    blit_image.BlitColor(framebuffer, texture.RenderTarget(), texture.ImageHandle(),
                         sampler->Handle(), dst_region, src_region, src_size);
    // The blit bound its own pipeline, viewport and scissor.
    scheduler.InvalidateState();
}

void RasterizerVulkan::Clear(u32 layer_count) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Clear};
    if (Tegra::Engines::g_nx2_trace_draws_after_upscale > 0) {
        const auto& regs = maxwell3d->regs;
        LOG_INFO(HW_GPU, "after upscale: clear rt0 {:#x} {}x{} fmt {:#x} surface {:#x} colour {} {} {} {}",
                 regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height,
                 static_cast<u32>(regs.rt[0].format), regs.clear_surface.raw, regs.clear_color[0],
                 regs.clear_color[1], regs.clear_color[2], regs.clear_color[3]);
    }
    TraceWindowTarget(*maxwell3d, *gpu_memory, "clear", layer_count);
    {
        VideoCommon::Nx2Profile::Scope flush_scope{VideoCommon::Nx2Profile::Zone::ClearFlush};
        FlushWork();
        gpu_memory->FlushCaching();
    }

    auto& regs = maxwell3d->regs;
    const bool use_color = regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
                           regs.clear_surface.A;
    const bool use_depth = regs.clear_surface.Z;
    const bool use_stencil = regs.clear_surface.S;
    if (!use_color && !use_depth && !use_stencil) {
        return;
    }

    std::scoped_lock lock{texture_cache.mutex};
    const Framebuffer* const framebuffer = [this] {
        VideoCommon::Nx2Profile::Scope targets_scope{VideoCommon::Nx2Profile::Zone::ClearTargets};
        texture_cache.UpdateRenderTargets(true);
        return texture_cache.GetFramebuffer();
    }();
    if (frame_dump.Capturing()) {
        FrameDumpPass(framebuffer);
    }
    const VkExtent2D render_area = framebuffer->RenderArea();
    const u32 base_array_layer = regs.clear_surface.layer;
    const auto clear_layers =
        GetFramebufferClearLayers(base_array_layer, layer_count, framebuffer->Layers());
    if (clear_layers.count != layer_count) {
        LOG_DEBUG(Render_Vulkan,
                  "Framebuffer clear layers: base={}, requested={}, clipped={}, framebuffer={}",
                  base_array_layer, layer_count, clear_layers.count, framebuffer->Layers());
    }
    if (clear_layers.count == 0) {
        return;
    }

    constexpr bool ENABLE_DEFERRED_CLEAR = true;
    const bool color_full_channels = regs.clear_surface.R && regs.clear_surface.G &&
                                     regs.clear_surface.B && regs.clear_surface.A;
    const bool stencil_partial = use_stencil && framebuffer->HasAspectStencilBit() &&
                                 regs.stencil_front_mask != 0xFF && regs.stencil_front_mask != 0;
    const bool ds_used = use_depth || use_stencil;
    const bool ds_deferrable =
        !ds_used || ((!framebuffer->HasAspectDepthBit() || use_depth) &&
                     (!framebuffer->HasAspectStencilBit() || use_stencil) && !stencil_partial);
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    VkRect2D default_scissor{};
    default_scissor.offset.x = 0;
    default_scissor.offset.y = 0;
    default_scissor.extent.width = (std::numeric_limits<s32>::max)();
    default_scissor.extent.height = (std::numeric_limits<s32>::max)();

    VkClearRect clear_rect{
        .rect = regs.clear_control.use_scissor ? GetScissorState(regs, 0, up_scale, down_shift)
                                               : default_scissor,
        .baseArrayLayer = base_array_layer,
        .layerCount = clear_layers.count,
    };
    const auto clamp_rect_to_render_area = [render_area](VkRect2D& rect) -> bool {
        const auto clamp_axis = [](s32& offset, u32& extent, u32 limit) {
            auto clamp_offset = [&offset, limit]() {
                if (limit == 0) {
                    offset = 0;
                    return;
                }
                offset = std::clamp(offset, 0, static_cast<s32>(limit));
            };

            if (extent == 0) {
                clamp_offset();
                return;
            }
            if (offset < 0) {
                const u32 shrink = (std::min)(extent, static_cast<u32>(-offset));
                extent -= shrink;
                offset = 0;
            }
            if (limit == 0) {
                extent = 0;
                offset = 0;
                return;
            }
            if (offset >= s32(limit)) {
                offset = s32(limit);
                extent = 0;
                return;
            }
            const u64 end_coord = u64(offset) + extent;
            if (end_coord > limit) {
                extent = limit - u32(offset);
            }
        };

        clamp_axis(rect.offset.x, rect.extent.width, render_area.width);
        clamp_axis(rect.offset.y, rect.extent.height, render_area.height);
        return rect.extent.width != 0 && rect.extent.height != 0;
    };
    if (!clamp_rect_to_render_area(clear_rect.rect)) {
        return;
    }

    if (VideoCommon::RenderTrace::SampleFrame(render_trace_frame) && render_trace_clears++ < 2048) {
        const u32 target = regs.clear_surface.RT;
        LOG_DEBUG(Render_Vulkan,
                  "Render trace clear: frame={} docked={} color={} depth={} stencil={} "
                  "color_target={} color_address={:#x} depth_address={:#x} depth_value={} base_layer={} layers={} "
                  "framebuffer_layers={} rect=({},{} {}x{})",
                  render_trace_frame, Settings::IsDockedMode(), use_color, use_depth, use_stencil,
                  target, target < regs.rt.size() ? regs.rt[target].Address() : 0,
                  regs.zeta.Address(), regs.clear_depth,
                  clear_rect.baseArrayLayer, clear_rect.layerCount, framebuffer->Layers(),
                  clear_rect.rect.offset.x, clear_rect.rect.offset.y,
                  clear_rect.rect.extent.width, clear_rect.rect.extent.height);
    }

    const bool clear_covers_render_area =
        clear_rect.rect.offset.x == 0 && clear_rect.rect.offset.y == 0 &&
        clear_rect.rect.extent.width >= render_area.width &&
        clear_rect.rect.extent.height >= render_area.height;
    // A pass open on another framebuffer ends here either way: the clear can still become the
    // load op of this framebuffer's next pass (no load, no clear inside it).
    const bool other_pass_open =
        scheduler.IsRenderPassActive() && !scheduler.RenderPassMatches(framebuffer);
    const bool can_defer_clear =
        ENABLE_DEFERRED_CLEAR && (!regs.clear_control.use_scissor || clear_covers_render_area) &&
        clear_layers.covers_all_layers && (!scheduler.IsRenderPassActive() || other_pass_open) &&
        (!use_color || color_full_channels) && ds_deferrable;
    if (!can_defer_clear) {
        // A clear only writes the attachments.
        scheduler.RequestDrawRenderpass(framebuffer, pipeline_cache.UsesShaderObjects());
    } else if (other_pass_open) {
        scheduler.RequestOutsideRenderPassOperationContext();
    }

    query_cache.NotifySegment(true);
    // Deferred load-op clears have no render pass yet. Starting a query here
    // would begin it outside the pass and end it inside the next draw's pass.
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              !can_defer_clear && regs.zpass_pixel_count_enable);
    UpdateViewportsState(regs);

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasAspectColorBit(color_attachment)) {
        const auto format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        bool is_integer = IsPixelFormatInteger(format);
        bool is_signed = IsPixelFormatSignedInteger(format);
        size_t int_size = PixelComponentSizeBitsInteger(format);
        VkClearValue clear_value{};
        if (!is_integer) {
            std::memcpy(clear_value.color.float32, regs.clear_color.data(), regs.clear_color.size() * sizeof(f32));
        } else if (!is_signed) {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.uint32[i] = u32(f32(u64(int_size) << 1U) * regs.clear_color[i]);
        } else {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.int32[i] = s32(f32(s64(int_size - 1) << 1) * (regs.clear_color[i] - 0.5f));
        }

        if (color_full_channels) {
            if (can_defer_clear) {
                scheduler.DeferColorClear(framebuffer, color_attachment, clear_value);
            } else {
                scheduler.Record([color_attachment, clear_value, clear_rect](vk::CommandBuffer cmdbuf) {
                    const VkClearAttachment attachment{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .colorAttachment = color_attachment,
                        .clearValue = clear_value,
                    };
                    cmdbuf.ClearAttachments(attachment, clear_rect);
                });
            }
        } else {
            u8 color_mask = u8(regs.clear_surface.R | regs.clear_surface.G << 1 | regs.clear_surface.B << 2 | regs.clear_surface.A << 3);
            Region2D dst_region = {
                Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
                Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                         .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
            blit_image.ClearColor(framebuffer, color_mask, regs.clear_color, dst_region);
        }
    }

    if (!use_depth && !use_stencil) {
        return;
    }
    VkImageAspectFlags aspect_flags = 0;
    if (use_depth && framebuffer->HasAspectDepthBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (use_stencil && framebuffer->HasAspectStencilBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (aspect_flags == 0) {
        return;
    }

    if (use_stencil && framebuffer->HasAspectStencilBit() && regs.stencil_front_mask != 0xFF &&
        regs.stencil_front_mask != 0) {
        Region2D dst_region = {
            Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
            Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                     .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
        blit_image.ClearDepthStencil(framebuffer, use_depth, regs.clear_depth,
                                     u8(regs.stencil_front_mask), regs.clear_stencil,
                                     regs.stencil_front_func_mask, dst_region);
    } else if (can_defer_clear) {
        VkClearValue ds_value{};
        ds_value.depthStencil.depth = regs.clear_depth;
        ds_value.depthStencil.stencil = regs.clear_stencil;
        scheduler.DeferDepthStencilClear(framebuffer, ds_value);
    } else {
        scheduler.Record([clear_depth = regs.clear_depth, clear_stencil = regs.clear_stencil,
                          clear_rect, aspect_flags](vk::CommandBuffer cmdbuf) {
            VkClearAttachment attachment;
            attachment.aspectMask = aspect_flags;
            attachment.colorAttachment = 0;
            attachment.clearValue.depthStencil.depth = clear_depth;
            attachment.clearValue.depthStencil.stencil = clear_stencil;
            cmdbuf.ClearAttachments(attachment, clear_rect);
        });
    }
}

void RasterizerVulkan::DispatchCompute() {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::Compute};
    // A dispatch writing pages the guest reads back (Ocarina of Time: two compute results a
    // frame) gets their downloads recorded right behind it: a fence of their own, whose
    // submission waits for the next one (Scheduler::DeferFlush). At the next fence the copies
    // would queue behind every draw recorded meanwhile, and the read waits for all of them.
    // After the caches' locks are released. Only this dispatch's writes count: draws' wait for
    // the next fence.
    (void)buffer_cache.TakeReadPrefetchQueued();
    // EXPERIMENT (do not merge): EDEN_NX2_EXP_NO_EARLY_READ=1 leaves them to the next fence.
    const bool no_early_read =
        Common::Nx2Switches::Get(Common::Nx2Switches::Switch::NoEarlyRead) != 0;
    SCOPE_EXIT {
        if (buffer_cache.TakeReadPrefetchQueued() && !no_early_read) {
            fence_manager.SignalReference();
        }
    };
    ++nx2_dispatch_serial;
    FlushWork();
    gpu_memory->FlushCaching();
    if (frame_dump.Capturing()) {
        frame_dump.EndPass();
        frame_dump.DrainIfFull();
    }

    u64 native_key{};
    ComputePipeline* const pipeline{[&] {
        VideoCommon::Nx2Profile::Scope lookup_scope{VideoCommon::Nx2Profile::Zone::ComputeLookup};
        return pipeline_cache.CurrentComputePipeline(native_key);
    }()};
    if (!pipeline) {
        if (kepler_compute->IsNativeMode()) throw Shader::RuntimeError("Native compute pipeline unavailable");
        return;
    }
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    std::optional<ByteAddressedShadow> byte_addressed_shadow;
    buffer_cache.Nx2CollectListCopies(true);
    SCOPE_EXIT {
        buffer_cache.Nx2CollectListCopies(false);
        if (byte_addressed_shadow) {
            texture_cache_runtime.RetireByteAddressedShadow(*byte_addressed_shadow);
        }
    };
    if (VideoCommon::Nx2Profile::Scope configure_scope{
            VideoCommon::Nx2Profile::Zone::ComputeConfigure};
        !pipeline->Configure(*kepler_compute, *gpu_memory, scheduler, buffer_cache, texture_cache,
                             texture_cache_runtime, byte_addressed_shadow)) {
        if (kepler_compute->IsNativeMode()) throw Shader::RuntimeError("Native compute resources unavailable");
        return;
    }
    SCOPE_EXIT {
        if (!frame_dump.Capturing()) {
            return;
        }
        boost::container::small_vector<ImageView*, 8> written;
        for (const VideoCommon::ImageViewId id : pipeline->WrittenImages()) {
            written.push_back(&texture_cache.GetImageView(id));
        }
        frame_dump.CaptureDispatch(pipeline->ShaderHash(), written);
    };
    TracePendingCpuWrites(true, std::array{pipeline->ShaderHash()});
    if (frame_dump.CapturingSampled()) {
        boost::container::small_vector<FrameDump::Sampled, 16> sampled;
        for (const auto& image : pipeline->SampledImages()) {
            sampled.push_back({5, image.tic, &texture_cache.GetImageView(image.id),
                               &texture_cache.GetSampler(image.sampler)});
        }
        frame_dump.NoteDispatchSampled(pipeline->ShaderHash(), sampled);
    }
    if (frame_dump.CapturingBuffers()) {
        FrameDumpBuffers(true, std::array{pipeline->ShaderHash()});
    }

    const auto& qmd{kepler_compute->launch_description};
    auto indirect_address = kepler_compute->GetIndirectComputeAddress();
    const auto list_slot = kepler_compute->GetIndirectListSlot();
    const auto publish_collision_map = [&] {
        if (native_key != 0x6e0ae200ee9aa220ULL ||
            (qmd.const_buffer_enable_mask & 1) == 0) {
            return;
        }
        const auto& constants = qmd.const_buffer_config[0];
        if (constants.size < 0x3c) {
            return;
        }
        // WaterSurfaceCollisionHeightMap exposes its output handle in CB0.
        const u32 handle = gpu_memory->Read<u32>(constants.Address() + 0x38);
        const u32 index = Tegra::Texture::TexturePair(handle, qmd.linked_tsc != 0).first;
        texture_cache.PublishComputeCollisionImage(index);
    };
    // Earlier writes must be visible to the dispatch, its indirect arguments included. The barrier
    // is only recorded when the dispatch depends on what ran since the last one; with nothing but
    // dispatches since then, it only has to wait for compute work. The next render pass orders
    // what the dispatch writes (Scheduler::NoteComputeWrites).
    const auto decide_barrier = [&](bool unknown_accesses) -> std::optional<VkPipelineStageFlags> {
        VideoCommon::Nx2Profile::Scope barrier_scope{VideoCommon::Nx2Profile::Zone::ComputeBarrier};
        const bool other_work = scheduler.RecordedCommands() != dispatch_recorded_commands;
        dispatch_accesses.Clear();
        if (unknown_accesses || byte_addressed_shadow) {
            dispatch_accesses.MarkUnknown();
        } else {
            buffer_cache.ForEachComputeBufferRange([&](DAddr addr, u64 size, bool written) {
                if (written) {
                    dispatch_accesses.WriteBuffer(addr, size);
                } else {
                    dispatch_accesses.ReadBuffer(addr, size);
                }
            });
            for (const ComputePipeline::ImageAccess& access : pipeline->ImageAccesses()) {
                u64 handle{};
                std::memcpy(&handle, &access.image, sizeof(access.image));
                if (access.written) {
                    dispatch_accesses.WriteImage(handle);
                } else {
                    dispatch_accesses.ReadImage(handle);
                }
            }
        }
        using Decision = VideoCommon::HazardTracker::Decision;
        const Decision decision = dispatch_hazards.Next(other_work, dispatch_accesses);
        if (VideoCommon::Nx2Profile::Enabled()) {
            using VideoCommon::Nx2Profile::Counter;
            static constexpr std::array<Counter, static_cast<size_t>(Decision::Count)> counters{
                Counter::DispatchNoBarrier,  Counter::DispatchBarrierOther,
                Counter::DispatchBarrierUnknown, Counter::DispatchBarrierRaw,
                Counter::DispatchBarrierWaw, Counter::DispatchBarrierWar,
            };
            VideoCommon::Nx2Profile::Add(counters[static_cast<size_t>(decision)], 1);
        }
        scheduler.NoteComputeWrites();
        if (AllDispatchBarriers()) {
            return VkPipelineStageFlags{vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER};
        }
        if (decision == Decision::NoBarrier) {
            return std::nullopt;
        }
        return decision == Decision::OtherWork
                   ? VkPipelineStageFlags{vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER}
                   : VkPipelineStageFlags{VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    };
    if (frame_dump.CapturingBuffers()) {
        std::optional<std::array<u32, 3>> indirect_grid;
        if (indirect_address) {
            indirect_grid = std::array{gpu_memory->Read<u32>(*indirect_address),
                                       gpu_memory->Read<u32>(*indirect_address + 4),
                                       gpu_memory->Read<u32>(*indirect_address + 8)};
        }
        frame_dump.NoteLaunch(pipeline->ShaderHash(),
                              {qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z},
                              {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
                              indirect_address, indirect_grid);
    }
    if (list_slot) {
        // The grid is GPU-written list words (Nx2ListIndirect): the host GPU copied them into the
        // slot at the list's place (with the barriers that make them visible to indirect reads),
        // and dispatches from there.
        auto& slots = buffer_cache_runtime.Nx2Slots();
        if (!slots.Held(*list_slot)) [[unlikely]] {
            LOG_ERROR(Render_Vulkan, "Nx2 list slot {} dispatched without a hold", *list_slot);
            return;
        }
        const auto [args_buffer, args_offset] = slots.DispatchArguments(*list_slot);
        scheduler.RequestOutsideRenderPassOperationContext();
        const auto barrier = decide_barrier(false);
        scheduler.TraceComputePipeline(pipeline->ShaderHash());
        scheduler.RecordForDispatch([this, pipeline, byte_addressed_shadow, barrier,
                                     indirect_buffer = args_buffer,
                                     indirect_offset = args_offset](vk::CommandBuffer cmdbuf) {
            if (barrier) {
                RecordDispatchBarrier(cmdbuf, *barrier);
            }
            if (!pipeline->IsBound()) {
                return;
            }
            if (byte_addressed_shadow) {
                TextureCacheRuntime::CopyByteAddressedShadow(cmdbuf, *byte_addressed_shadow, false);
            }
            scheduler.TraceGpuWork(cmdbuf, pipeline->ShaderHash(), 0, {}, [&] {
                cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
            });
            if (byte_addressed_shadow) {
                TextureCacheRuntime::CopyByteAddressedShadow(cmdbuf, *byte_addressed_shadow, true);
            }
        });
        // A packed R16 shadow copies by transfer inside the dispatch's command (as below).
        dispatch_recorded_commands = byte_addressed_shadow ? ~u64{0} : scheduler.RecordedCommands();
        Tegra::Nx2ListIndirect::Stats::Count(Tegra::Nx2ListIndirect::Stats::Get().slot_dispatches);
        if (Tegra::Nx2ListIndirect::Stats::g_check) [[unlikely]] {
            slots.CheckDispatch(*list_slot); // TEST
        }
        SubmitNx2ListCopies();
        publish_collision_map();
        return;
    }
    if (indirect_address) {
        // DispatchIndirect
        static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
        const auto post_op = VideoCommon::ObtainBufferOperation::DiscardWrite;
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(*indirect_address, 12, sync_info, post_op);
        scheduler.RequestOutsideRenderPassOperationContext();
        // The arguments' buffer is not tracked: the barrier as before.
        const auto barrier = decide_barrier(true);
        scheduler.TraceComputePipeline(pipeline->ShaderHash());
        scheduler.RecordForDispatch([this, pipeline, indirect_buffer = buffer->Handle(),
                                     indirect_offset = offset,
                                     barrier](vk::CommandBuffer cmdbuf) {
            if (barrier) {
                RecordDispatchBarrier(cmdbuf, *barrier);
            }
            if (!pipeline->IsBound()) {
                return;
            }
            scheduler.TraceGpuWork(cmdbuf, pipeline->ShaderHash(), 0, {}, [&] {
                cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
            });
        });
        dispatch_recorded_commands = scheduler.RecordedCommands();
        SubmitNx2ListCopies();
        publish_collision_map();
        return;
    }
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    const std::array<u32, 3> max_dim{device.GetMaxComputeWorkGroupCount()};
    if (dim[0] > max_dim[0] || dim[1] > max_dim[1] || dim[2] > max_dim[2]) {
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    const auto barrier = decide_barrier(false);
    scheduler.TraceComputePipeline(pipeline->ShaderHash());
    scheduler.RecordForDispatch([this, pipeline, dim, byte_addressed_shadow,
                                 barrier](vk::CommandBuffer cmdbuf) {
        if (barrier) {
            RecordDispatchBarrier(cmdbuf, *barrier);
        }
        if (!pipeline->IsBound()) {
            return;
        }
        if (byte_addressed_shadow) {
            TextureCacheRuntime::CopyByteAddressedShadow(cmdbuf, *byte_addressed_shadow, false);
        }
        scheduler.TraceGpuWork(cmdbuf, pipeline->ShaderHash(), 0, dim,
                               [&] { cmdbuf.Dispatch(dim[0], dim[1], dim[2]); });
        if (byte_addressed_shadow) {
            TextureCacheRuntime::CopyByteAddressedShadow(cmdbuf, *byte_addressed_shadow, true);
        }
    });
    // A packed R16 shadow copies by transfer inside the dispatch's command: the next dispatch
    // counts it as other work.
    dispatch_recorded_commands =
        byte_addressed_shadow ? ~u64{0} : scheduler.RecordedCommands();
    SubmitNx2ListCopies();
    publish_collision_map();

    // Log compute dispatch
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string params = fmt::format("groupCountX={}, groupCountY={}, groupCountZ={}",
            dim[0], dim[1], dim[2]);
        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            "vkCmdDispatch", params, VK_SUCCESS);
    }
}

void RasterizerVulkan::ResetCounter(VideoCommon::QueryType type) {
    switch (type) {
    case VideoCommon::QueryType::ZPassPixelCount64:
    case VideoCommon::QueryType::StreamingByteCount:
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
    case VideoCommon::QueryType::VtgPrimitivesOut:
        query_cache.CounterReset(type);
        return;
    default:
        LOG_DEBUG(Render_Vulkan, "Unimplemented counter reset={}", type);
        return;
    }
}

void RasterizerVulkan::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                             VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
}

void RasterizerVulkan::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                 u32 size) {
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void Vulkan::RasterizerVulkan::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerVulkan::FlushAll() {}

void RasterizerVulkan::SubmitNx2ListCopies() {
    // ReadGpuWrittenCommands waits on these copies, so they can't sit out the longer DeferFlush
    // interval (#161). The front reads few of the lists (Ocarina: ~150 dispatches a frame, a
    // submission each): they are submitted when it waits for one (IsGpuTickDone and
    // WaitSubmittedTick ask the back), or with the next submission.
    if (buffer_cache.Nx2CopyListPages(scheduler.CurrentTick())) {
        if (scheduler.DeferFlushUntilWanted()) {
            return;
        }
        scheduler.Flush();
    }
}

VideoCore::RasterizerInterface::GpuWrittenList RasterizerVulkan::ReadGpuWrittenCommandList(
    DAddr addr, std::span<u8> out, bool past_sync, u64* pending_tick) {
    // No cache lock: the front only reads the copies, under their own lock.
    switch (buffer_cache.Nx2ReadListCopy(addr, out, past_sync, pending_tick)) {
    case BufferCache::Nx2ListRead::Copied:
        return GpuWrittenList::Copied;
    case BufferCache::Nx2ListRead::Synced:
        return GpuWrittenList::Synced;
    case BufferCache::Nx2ListRead::NeedsSync:
        return GpuWrittenList::NeedsSync;
    case BufferCache::Nx2ListRead::Pending:
        return GpuWrittenList::Pending;
    default:
        return GpuWrittenList::Unknown;
    }
}

bool RasterizerVulkan::IsGpuTickDone(u64 tick) {
    if (scheduler.IsFree(tick)) {
        return true;
    }
    // A deferred submission (Scheduler::DeferFlush) is only made by the split's back: ask it to
    // submit, or the tick never completes while the front polls.
    if (!scheduler.IsSubmitted(tick) && VideoCommon::GpuSplit::Get().Active()) {
        VideoCommon::GpuSplit::Get().RequestIdleHook();
    }
    scheduler.GetMasterSemaphore().Refresh();
    return scheduler.IsFree(tick);
}

void RasterizerVulkan::NoteGpuWrittenCommandListSynced(DAddr page) {
    buffer_cache.Nx2NoteListSynced(page);
}

bool RasterizerVulkan::Nx2ListSlotsSupported() const {
    // A direct dispatch skips a grid over the device's limits; an indirect one must not get one.
    // The slot's height and depth are 16 bits, and a width over 31 bits is no culling result.
    const std::array<u32, 3> max_dim{device.GetMaxComputeWorkGroupCount()};
    const bool fits = max_dim[0] >= 0x7fffffffU && max_dim[1] >= 0xffffU && max_dim[2] >= 0xffffU;
    if (static std::atomic<bool> logged{}; !fits && !logged.exchange(true)) {
        LOG_WARNING(Render_Vulkan,
                    "Nx2 list indirect off: the device dispatches at most {}x{}x{} groups",
                    max_dim[0], max_dim[1], max_dim[2]);
    }
    return Tegra::Nx2ListIndirect::g_enabled && fits;
}

std::optional<u32> RasterizerVulkan::Nx2AcquireListSlot() {
    return buffer_cache_runtime.Nx2Slots().Acquire();
}

void RasterizerVulkan::Nx2ReleaseListSlot(u32 slot) {
    buffer_cache_runtime.Nx2Slots().Release(slot);
}

void RasterizerVulkan::Nx2SnapshotListWords(DAddr device_addr, u32 count, u32 slot) {
    // The words as GPU work sent before the list left them: the buffer cache's, its CPU writes
    // applied (as a dispatch reading them finds them).
    gpu_memory->FlushCaching();
    std::scoped_lock lock{buffer_cache.mutex};
    const auto [buffer, offset] =
        buffer_cache.ObtainCPUBuffer(device_addr, count * static_cast<u32>(sizeof(u32)),
                                     VideoCommon::ObtainBufferSynchronize::FullSynchronize,
                                     VideoCommon::ObtainBufferOperation::DoNothing);
    buffer_cache_runtime.Nx2Slots().Snapshot(buffer->Handle(), offset, count, slot);
}

void RasterizerVulkan::Nx2NoteOwedListWord(DAddr device_addr, u32 value) {
    buffer_cache_runtime.Nx2Slots().NoteOwed(device_addr, value,
                                             device_memory.Read<u32>(device_addr));
}

void RasterizerVulkan::Nx2NoteInlineQmdGrid(DAddr device_addr, u32 x, u32 y, u32 z) {
    buffer_cache_runtime.Nx2Slots().NoteInlineQmdGrid(device_addr, x, y, z);
}

void RasterizerVulkan::Nx2ReadListSlots(std::span<const u32> slots, std::span<u32> words) {
    buffer_cache_runtime.Nx2Slots().Read(slots, words);
}

void RasterizerVulkan::Nx2CheckListSlot(u32 slot, std::span<const u32> words) {
    buffer_cache_runtime.Nx2Slots().CheckSnapshot(slot, words); // TEST
}

void RasterizerVulkan::FlushGpuWrittenCommandListPages(std::span<const DAddr> pages) {
    gpu.InvalidateGPUCache();
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.Nx2FlushListPages(pages);
}

void RasterizerVulkan::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & (VideoCommon::CacheType::TextureCache | VideoCommon::CacheType::BufferCache))) {
        // CPU writes into GPU-written memory are only collected when they happen. Apply them
        // first, or the download writes the stale GPU copy over them: the Yoshi demo writes a
        // native program over memory a compute pass had bound as a written texel buffer, and its
        // title menu or background read as zeros once the shader cache's read flushed it.
        gpu.InvalidateGPUCache();
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.FlushRegion(addr, size);
    }
}

bool RasterizerVulkan::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    // The split's front asks this before each macro DMA read, and the back holds the buffer
    // cache's lock through each draw: pages the GPU never wrote need no look under it.
    if (True(which & VideoCommon::CacheType::BufferCache) &&
        (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) ||
         Tegra::Nx2MayBeGpuWritten(addr, size))) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        // The texture cache only flushes or reports GPU-modified images: pages no image was ever
        // GPU-modified on skip its lock.
        if (!texture_cache.MayHaveGpuModifiedImages(addr, size)) {
            return false;
        }
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

bool RasterizerVulkan::HasPendingQueries(DAddr addr, u64 size) {
    return query_cache.IsRegionGpuModified(addr, size);
}

VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {
    std::optional<VideoCore::RasterizerDownloadArea> texture_area;
    // Guest cores ask this on reads of GPU-cached pages, and the back thread holds the texture
    // cache's lock through each draw: where no image was ever GPU-modified it has no area.
    if (texture_cache.MayHaveGpuModifiedImages(addr, size)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_area = texture_cache.GetFlushArea(addr, size);
    }
    if (texture_area) {
        // The core disarmed the read's pages before asking, and skips later reads in this area,
        // even through another device alias of those pages. The buffer cache arms only pages it
        // doesn't count as armed, so it must forget the whole area.
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.ForgetReadTraps(texture_area->start_address,
                                     texture_area->end_address - texture_area->start_address);
        return *texture_area;
    }
    // And pages never GPU-written or read-trapped (with Ampere's GPU-write marks) need no look
    // into the buffer cache, whose lock the back holds through each draw: a clean area.
    const bool skip_buffers = Tegra::g_nx2_ampere.load(std::memory_order_relaxed) &&
                              !device_memory.MayHaveArmedReadTraps(addr, size) &&
                              !Tegra::Nx2MayBeGpuWritten(addr, size);
    if (!skip_buffers && buffer_cache.FlushesOnCpuRead()) {
        std::scoped_lock lock{buffer_cache.mutex};
        auto area = buffer_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    VideoCore::RasterizerDownloadArea new_area{
        .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}

namespace {
/// The buffer cache's queue of a range's GPU-written bytes for the next fence's downloads
/// (BufferCache::Nx2QueueReadDownload), where the cache has it.
template <typename Cache>
bool QueueBufferReadDownload(Cache& cache, DAddr addr, u64 size) {
    if constexpr (requires { cache.Nx2QueueReadDownload(addr, size); }) {
        return cache.Nx2QueueReadDownload(addr, size);
    } else {
        return false;
    }
}
} // Anonymous namespace

bool RasterizerVulkan::QueueFlushRegion(DAddr addr, u64 size) {
    // A guest read of GPU-written buffer data that no fence download covered (~34 a second in
    // Ocarina, each page read a few times a minute): the flush waited for the host GPU to finish
    // all work submitted so far (Scheduler::Finish, ~0.4 ms) on this thread. Instead the range is
    // downloaded at a fence made and submitted now, as prefetched data is, and the reader waits
    // for it to land: the copy holds the same data (what the GPU wrote up to this point), and
    // this thread goes on.
    if (addr == 0 || size == 0) {
        return false;
    }
    {
        // Image data is flushed as before.
        std::scoped_lock lock{texture_cache.mutex};
        if (texture_cache.IsRegionGpuModified(addr, size)) {
            return false;
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (!QueueBufferReadDownload(buffer_cache, addr, size)) {
            return false;
        }
    }
    fence_manager.SignalReference();
    scheduler.Flush();
    return true;
}

void RasterizerVulkan::PrepareRegionWrite(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0 || False(which & VideoCommon::CacheType::BufferCache)) {
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.ClearDownload(addr, size);
}

void RasterizerVulkan::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (addr == 0 || size == 0) {
        return;
    }
    // Device unmaps come here before the pages lose their backing (DeviceMemoryManager::Unmap):
    // the watch lets go of its protection then, as the caches do, or the page's cached count
    // would outlive the mapping.
    VideoCommon::GetDrawInputWatch().ReleaseRange(addr, size);
    VideoCommon::GetDrawInputWatch().NoteHostWrite(addr, size);
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.InvalidateRegion(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::ShaderCache))) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    auto& watch = VideoCommon::GetDrawInputWatch();
    for (const auto& [addr, size] : sequences) {
        watch.NoteHostWrite(addr, size);
    }
    // The GPU's own cached writes (constant buffer data, every draw): most land where no image,
    // query or shader was ever registered, which page_filter tells without the caches' locks.
    const auto any = [&](const VideoCommon::PageFilter& filter) {
        for (const auto& [addr, size] : sequences) {
            if (filter.Any(addr, size)) {
                return true;
            }
        }
        return false;
    };
    // Even with skip_cpu_inner_invalidation: engines writing images in software (DMA pitch to
    // block linear copies of decoded video frames) reach the texture cache only through here.
    if (any(texture_cache.page_filter)) {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            texture_cache.WriteMemory(addr, size);
        }
    }
    if (Settings::values.skip_cpu_inner_invalidation.GetValue()) {
        return;
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            buffer_cache.WriteMemory(addr, size);
        }
    }
    if (any(query_cache.page_filter)) {
        for (const auto& [addr, size] : sequences) {
            query_cache.InvalidateRegion(addr, size);
        }
    }
    // Shaders marked by an earlier CPU write are removed here as before.
    if (any(pipeline_cache.page_filter) || pipeline_cache.HasPendingRemovals()) {
        pipeline_cache.InvalidateRegions(sequences);
    }
}

bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {
    DEBUG_ASSERT(addr != 0 || size != 0);
    // Guest write traps skip the texture cache's lock and the shader cache where their page
    // filters prove nothing was ever registered (as InnerInvalidation).
    // The GPU thread holds the buffer cache's lock through each draw. On pages the GPU never
    // wrote (Ampere's marks) the answer is "not GPU-modified", so the trap returns false either
    // way; its buffer part (WriteMemory) is queued for the lock's holder, which applies it before
    // anyone else uses the buffer cache (the constructor's owner hook). Pages with images or
    // shaders keep the usual path.
    const bool handoff = Tegra::g_nx2_ampere.load(std::memory_order_relaxed) &&
                         !Tegra::Nx2MayBeGpuWritten(addr, size) &&
                         !texture_cache.page_filter.Any(addr, size) &&
                         !pipeline_cache.page_filter.Any(addr, size);
    if (handoff && !buffer_cache.mutex.try_lock()) {
        if (write_handoff.Push(addr, size)) {
            // The GPU may have marked the page meanwhile, before the queue was visible: then it
            // may also download over it before a holder sees the queue. Wait for the holder as
            // before; taking the lock applies the queue.
            if (Tegra::Nx2MayBeGpuWritten(addr, size)) {
                std::scoped_lock lock{buffer_cache.mutex};
            }
            if (pipeline_cache.HasPendingRemovals()) {
                pipeline_cache.InvalidateRegion(addr, size);
            }
            return false;
        }
        buffer_cache.mutex.lock();
    } else if (!handoff) {
        buffer_cache.mutex.lock();
    }
    {
        std::unique_lock lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }
    if (texture_cache.page_filter.Any(addr, size)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if (pipeline_cache.page_filter.Any(addr, size) || pipeline_cache.HasPendingRemovals()) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
    return false;
}

void RasterizerVulkan::OnCacheInvalidations(std::span<const std::pair<DAddr, u64>> ranges) {
    // Out of the GPU-written ranges first: applying one write downloads the GPU's bytes elsewhere
    // on its pages (WriteMemory), which would write over the next collected write.
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.ClaimCpuWrites(ranges);
    }
    for (const auto& [addr, size] : ranges) {
        OnCacheInvalidation(addr, size);
    }
}

void RasterizerVulkan::OnCacheInvalidation(DAddr addr, u64 size) {
    if (addr == 0 || size == 0) {
        return;
    }
    VideoCommon::GetDrawInputWatch().NoteHostWrite(addr, size);

    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

bool RasterizerVulkan::TryCacheInvalidation(DAddr addr, u64 size) {
    if (addr == 0 || size == 0) return true;
    VideoCommon::GetDrawInputWatch().NoteHostWrite(addr, size);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        // WriteMemory downloads GPU-written bytes elsewhere on the written pages, which only the
        // GPU thread may do: leave those pages to the fallback.
        const DAddr page_begin = Common::AlignDown(addr, Core::DEVICE_PAGESIZE);
        const DAddr page_end = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE);
        if (buffer_cache.IsRegionGpuModified(page_begin, page_end - page_begin)) return false;
        texture_cache.WriteMemory(addr, size);
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return true;
}

void RasterizerVulkan::InvalidateGPUCache() {
    gpu.InvalidateGPUCache();
}

void RasterizerVulkan::UnmapMemory(DAddr addr, u64 size) {
    VideoCommon::GetDrawInputWatch().ReleaseRange(addr, size);
    VideoCommon::GetDrawInputWatch().NoteHostWrite(addr, size);
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size, false);
    }
    pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerVulkan::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapGPUMemory(as_id, addr, size);
    }
    buffer_cache.UnmapGPUMemory(as_id, addr, size);
}

void RasterizerVulkan::SignalFence(std::function<void()>&& func) {
    fence_manager.SignalFence(std::move(func));
}

void RasterizerVulkan::SyncOperation(std::function<void()>&& func) {
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerVulkan::SignalSyncPoint(u32 value) {
    fence_manager.SignalSyncPoint(value);
}

void RasterizerVulkan::SignalReference() {
    fence_manager.SignalReference();
}

void RasterizerVulkan::ReleaseFences(bool force) {
    fence_manager.WaitPendingFences(force);
}

bool RasterizerVulkan::FencesQuiet(u64& epoch) {
    return fence_manager.FencesQuiet(epoch);
}

u64 RasterizerVulkan::FenceWorkEpoch() const {
    return fence_manager.FenceWorkEpoch();
}

void RasterizerVulkan::FlushQueryResults(DAddr addr, u64 size) {
    if (addr == 0 || size == 0 || !query_cache.SemiFlushRegion(addr, size)) {
        return;
    }
    // The fence carries the reports still waiting for one; on the split's back its submission
    // may be deferred, and the fencing thread would wait for this thread to submit it.
    fence_manager.SignalFence([] {});
    scheduler.FlushDeferred();
    // Wait for the reports themselves, not for an operation queued behind everything: the work
    // submitted after them (a few ms of GPU time while walking) cannot write them.
    Tegra::Nx2WaitAsyncReportsSettled(addr, size);
}

void RasterizerVulkan::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                                VideoCommon::CacheType which) {
    if (Settings::IsGPULevelHigh()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerVulkan::WaitForIdle() {
    // Everything but wait pixel operations. This intentionally includes FRAGMENT_SHADER_BIT because
    // fragment shaders can still write storage buffers.
    VkPipelineStageFlags flags =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
    }

    query_cache.NotifyWFI();

    if (device.IsMoltenVK() && scheduler.IsRenderPassActive()) {
        scheduler.DeferFragmentBarrier();
        fence_manager.SignalOrdering();
        return;
    }
    // A wait for idle between draws of one render pass ended it (~1,200 a second in Ocarina).
    // Since the pass began only its draws ran; their attachment writes are covered by the
    // deferred fragment barrier (the next draw sampling a target ends the pass) and by the
    // barrier at the pass end. Storage or transform feedback writes, or a dispatch since the
    // last real wait (dispatches have no barrier after them), still wait here.
    if (scheduler.IsRenderPassActive() && !scheduler.RenderPassHasStores() &&
        nx2_dispatch_serial == nx2_wfi_dispatch_serial &&
        maxwell3d->regs.transform_feedback_enabled == 0) {
        scheduler.DeferFragmentBarrier();
        fence_manager.SignalOrdering();
        return;
    }
    nx2_wfi_dispatch_serial = nx2_dispatch_serial;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([flags](vk::CommandBuffer cmdbuf) {
        // Later work waits for the earlier work and sees its writes.
        static constexpr VkMemoryBarrier WFI_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(flags, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WFI_BARRIER);
    });
    fence_manager.SignalOrdering();
}

void RasterizerVulkan::FragmentBarrier() {
    // The next draw breaks the render pass only if it reads what this one wrote (#161): Ocarina
    // ended ~5,900 render passes a second here.
    scheduler.DeferFragmentBarrier();
}

void RasterizerVulkan::TiledCacheBarrier() {
    // TODO: Implementing tiled barriers requires rewriting a good chunk of the Vulkan backend
}

void RasterizerVulkan::FlushCommands() {
    if (draw_counter == 0 && !pending_native_work) {
        return;
    }
    draw_counter = 0;
    pending_native_work = false;
    // Fences with downloads or queries come here right after queueing: ~9,400 a second in
    // Hyrule Warriors: Age of Imprisonment fights. Batched like the fences themselves.
    if (!scheduler.DeferFlush()) {
        scheduler.Flush();
    }
}

void RasterizerVulkan::TraceRenderState(bool is_indexed, const char* outcome,
                                        const Framebuffer* framebuffer) {
    if (!VideoCommon::RenderTrace::SampleFrame(render_trace_frame)) {
        return;
    }
    if (render_trace_draws++ >= 2048) {
        if (render_trace_draws == 2049) {
            LOG_DEBUG(Render_Vulkan, "Render trace frame={} truncated after 2048 draws",
                      render_trace_frame);
        }
        return;
    }
    const auto& regs = maxwell3d->regs;
    const auto& draw = maxwell3d->draw_manager.draw_state;
    std::array<u64, 6> shaders{};
    u32 stages{};
    for (size_t stage = 0; stage < shaders.size(); ++stage) {
        shaders[stage] = (u64{regs.pipelines[stage].reserved[0] & 0xff} << 32) |
                         regs.pipelines[stage].reserved[1];
        stages |= static_cast<u32>(regs.IsShaderConfigEnabled(stage)) << stage;
    }
    const auto extent = framebuffer ? framebuffer->RenderArea() : VkExtent2D{};
    LOG_DEBUG(Render_Vulkan,
              "Render trace frame={} draw={} docked={} outcome={} indexed={} stages={:#x} "
              "shaders={:x} guest_count={} guest_instances={} framebuffer={}x{} layers={}",
              render_trace_frame, render_trace_draws, Settings::IsDockedMode(), outcome, is_indexed,
              stages, fmt::join(shaders, ","),
              is_indexed ? draw.index_buffer.count : draw.vertex_buffer.count, draw.instance_count,
              extent.width, extent.height, framebuffer ? framebuffer->Layers() : 0);
    LOG_DEBUG(Render_Vulkan,
              "Render trace targets: color={:#x} {}x{} layers={} format={:#x} depth={:#x} "
              "{}x{} layers={} format={:#x} enabled={} clip={}x{}",
              regs.rt[0].Address(), regs.rt[0].width, regs.rt[0].height, regs.rt[0].depth.Value(),
              static_cast<u32>(regs.rt[0].format), regs.zeta.Address(), regs.zeta_size.width,
              regs.zeta_size.height, regs.zeta_size.depth.Value(), static_cast<u32>(regs.zeta.format),
              regs.zeta_enable, regs.surface_clip.width.Value(), regs.surface_clip.height.Value());
    const auto& viewport = regs.viewport_transform[0];
    const auto& scissor = regs.scissor_test[0];
    LOG_DEBUG(Render_Vulkan,
              "Render trace tests: raster={} depth_test={} depth_write={} depth_func={} "
              "cull={} face={} alpha={} func={} ref={} color_mask={:#x} "
              "viewport_scale=({},{},{}) translate=({},{},{}) scissor={} {}..{} {}..{}",
              regs.rasterize_enable, regs.depth_test_enable, regs.depth_write_enabled,
              static_cast<u32>(regs.depth_test_func), regs.gl_cull_test_enabled,
              static_cast<u32>(regs.gl_cull_face), regs.alpha_test_enabled,
              static_cast<u32>(regs.alpha_test_func), regs.alpha_test_ref, regs.color_mask[0].raw,
              viewport.scale_x, viewport.scale_y, viewport.scale_z, viewport.translate_x,
              viewport.translate_y, viewport.translate_z, scissor.enable, scissor.min_x.Value(),
              scissor.max_x.Value(), scissor.min_y.Value(), scissor.max_y.Value());
    for (size_t binding = 0; binding < regs.vertex_streams.size(); ++binding) {
        const auto& stream = regs.vertex_streams[binding];
        if (stream.IsEnabled()) {
            LOG_DEBUG(Render_Vulkan,
                      "Render trace vertex: binding={} address={:#x} limit={:#x} stride={} "
                      "frequency={} repacked={}",
                      binding, stream.Address(), regs.vertex_stream_limits[binding].Address(),
                      stream.stride.Value(), stream.frequency,
                      buffer_cache_runtime.IsVertexRepacked(static_cast<u32>(binding)));
        }
    }
    if (is_indexed) {
        LOG_DEBUG(Render_Vulkan, "Render trace index: address={:#x} limit={:#x} format={} first={}",
                  regs.index_buffer.StartAddress(), regs.index_buffer.EndAddress(),
                  static_cast<u32>(regs.index_buffer.format), draw.index_buffer.first);
    }
    LOG_DEBUG(Render_Vulkan, "Render trace condition: address={:#x} mode={} override={}",
              regs.render_enable.Address(), static_cast<u32>(regs.render_enable.mode),
              static_cast<u32>(regs.render_enable_override));
}

void RasterizerVulkan::TracePendingCpuWrites(bool compute, std::span<const u64> shader_hashes) {
    // Guest CPU writes into GPU-written buffer memory are collected, and applied to the host
    // buffers only at the next fence or cache invalidation. A hit here reads the older bytes.
    static const bool enabled = std::getenv("EDEN_NX2_PENDING_WRITE_TRACE") != nullptr;
    if (!enabled) {
        return;
    }
    static constexpr u64 max_lines = 100000;
    static u64 lines{};
    buffer_cache.ForEachBoundBuffer(compute, [&](u32 stage, bool storage, u32 index, bool written,
                                                 const auto& binding, u32 bound_size,
                                                 const auto*, u32) {
        if (lines > max_lines || !gpu.HasPendingCPUWrites(binding.device_addr, bound_size)) {
            return;
        }
        if (++lines > max_lines) {
            LOG_INFO(HW_GPU, "Pending write trace stopped after {} lines", max_lines);
            return;
        }
        const size_t program = compute ? 0 : stage + 1;
        LOG_INFO(HW_GPU,
                 "Pending CPU write frame={} {} shader={:016x} stage={} {}[{}]{} addr={:#x} "
                 "device={:#x} size={:#x}",
                 render_trace_frame, compute ? "dispatch" : "draw",
                 program < shader_hashes.size() ? shader_hashes[program] : 0, stage,
                 storage ? "ssbo" : "cbuf", index, written ? " written" : "", binding.gpu_addr,
                 binding.device_addr, bound_size);
    });
}

bool RasterizerVulkan::FrameDumpBuffers(bool compute, std::span<const u64> shader_hashes) {
    static constexpr u32 BUFFER_CAP = 4U << 20;
    std::vector<FrameDump::BoundBuffer> buffers;
    buffer_cache.ForEachBoundBuffer(compute, [&](u32 stage, bool storage, u32 index, bool written,
                                                 const auto& binding, u32 bound_size,
                                                 const auto* host, u32 host_offset) {
        const u32 size = std::min(bound_size, BUFFER_CAP);
        const bool gpu_modified = buffer_cache.IsRegionGpuModified(binding.device_addr, size);
        FrameDump::BoundBuffer& buffer = buffers.emplace_back(FrameDump::BoundBuffer{
            .stage = stage,
            .storage = storage,
            .index = index,
            .written = written,
            .gpu_addr = binding.gpu_addr,
            .device_addr = binding.device_addr,
            .bound_size = bound_size,
            .size = size,
            .gpu_modified = gpu_modified,
            .host = host ? host->Handle() : VK_NULL_HANDLE,
            .host_offset = host_offset,
            .guest = {},
        });
        if (!gpu_modified || !host) {
            buffer.guest.resize(size);
            device_memory.ReadBlockUnsafe(binding.device_addr, buffer.guest.data(), size);
        }
    });
    return frame_dump.NoteBuffers(compute, shader_hashes, buffers);
}

void RasterizerVulkan::FrameDumpPass(const Framebuffer* framebuffer) {
    std::array<ImageView*, VideoCommon::NUM_RT> colors{};
    const auto& render_targets = texture_cache.GetRenderTargets();
    const auto& ids = render_targets.color_buffer_ids;
    for (size_t slot = 0; slot < ids.size(); ++slot) {
        if (ids[slot]) {
            colors[slot] = &texture_cache.GetImageView(ids[slot]);
        }
    }
    ImageView* const depth = render_targets.depth_buffer_id
                                 ? &texture_cache.GetImageView(render_targets.depth_buffer_id)
                                 : nullptr;
    if (frame_dump.BeginPass(framebuffer, colors, depth)) {
        scheduler.RequestRenderpass(framebuffer);
    }
}

void RasterizerVulkan::TickFrame() {
    // EXPERIMENT (do not merge): A/B switches flipped by the debug server take effect here,
    // between frames (common/nx2_switches.h).
    Common::Nx2Switches::Latch(draw_watch_frame + 1);
    {
        auto& watch = VideoCommon::GetDrawInputWatch();
        watch.Sweep(++draw_watch_frame);
        if (VideoCommon::DrawClass::Enabled()) {
            const auto now = std::chrono::steady_clock::now();
            const double seconds =
                std::chrono::duration<double>(now - draw_watch_report).count();
            if (seconds >= 10.0) {
                watch.Report(seconds);
                draw_watch_report = now;
            }
        }
    }
    if (oot_object_motion) {
        oot_object_motion->EnsureImage(scheduler, OotObjectMotionSize());
    }
    if (VideoCommon::OotDlss::Requested()) {
        const u64 frame = VideoCommon::OotDlss::FrameCounter().fetch_add(1, std::memory_order_relaxed);
        if (frame % 600 == 599) {
            LOG_INFO(Render_Vulkan, "OoT DLSS: {} jittered viewport updates in the last 600 frames",
                     std::exchange(oot_jittered_updates, 0));
        }
    }
    if (VideoCore::Nx2Debug::HasRequests()) {
        if (auto capture = VideoCore::Nx2Debug::TakeCapture()) {
            frame_dump.Arm(std::move(capture->directory), capture->sampled, capture->buffers,
                           capture->id);
        }
        if (const auto change = VideoCore::Nx2Debug::TakeRejectChange()) {
            pipeline_cache.RefreshDebugRejections(change->unrejected);
            VideoCore::Nx2Debug::NoteRejectApplied(change->generation);
        }
        while (const auto read = VideoCore::Nx2Debug::TakeGpuRead()) {
            std::vector<Tegra::MemoryManager*> spaces;
            if (gpu_memory != nullptr) {
                spaces.push_back(gpu_memory);
            }
            {
                std::scoped_lock lock{config_mutex};
                for (const auto& [id, space] : address_spaces) {
                    if (space.gpu_memory != gpu_memory) {
                        spaces.push_back(space.gpu_memory);
                    }
                }
            }
            VideoCore::Nx2Debug::FinishGpuRead(read->id,
                                               ReadGpuMemory(spaces, read->gpu_address, read->size));
        }
    }
    frame_dump.TickFrame();
    ++render_trace_frame;
    render_trace_draws = 0;
    render_trace_clears = 0;
    if (Common::DkbFrameTrace::Enabled()) {
        CollectIndirectTrace();
        const auto now = std::chrono::steady_clock::now();
        if (trace_cpu_rate.Ready(now)) {
            auto snapshot = Common::DkbCpuTrace::ReadSnapshot();
            if (trace_cpu_snapshot && trace_frame > trace_cpu_frame) {
                const u64 frames = trace_frame - trace_cpu_frame;
                std::string roles;
                for (size_t i = 0; i < Common::DkbCpuTrace::RoleCount; ++i) {
                    const auto name = Common::DkbCpuTrace::RoleNames[i];
                    const auto delta = Common::DkbCpuTrace::CpuTimeDelta(
                        trace_cpu_snapshot->threads[i], snapshot.threads[i]);
                    if (delta) {
                        const double cpu_ms = static_cast<double>(*delta) / 1e6;
                        roles += fmt::format(" {}_cpu_ms={:.3f} {}_cpu_ms_per_frame={:.3f}",
                                             name, cpu_ms, name, cpu_ms / frames);
                    } else {
                        roles += fmt::format(" {}={}", name,
                                             snapshot.threads[i].active ? "unavailable" : "inactive");
                    }
                }
                LOG_INFO(Render_Vulkan,
                         "DKB cpu through_frame={} frames={} window_wall_ms={:.3f}{}",
                         trace_frame, frames,
                         std::chrono::duration<double, std::milli>(
                             snapshot.wall_time - trace_cpu_snapshot->wall_time).count(),
                         roles);
            }
            trace_cpu_snapshot = std::move(snapshot);
            trace_cpu_frame = trace_frame;
        }
        ++trace_window_frames;
        if (trace_sample_frame) {
            LOG_INFO(Render_Vulkan,
                     "DKB draws through_frame={} frames={} attempts={} skipped={} indirect={} "
                     "zero_direct={} capture_dropped={} attempts_per_frame={:.1f} "
                     "zero_direct_per_frame={:.1f} (totals since previous line)",
                     trace_frame, trace_window_frames, trace_draws, trace_skipped, trace_indirect,
                     trace_zero_direct, trace_capture_dropped,
                     static_cast<double>(trace_draws) / trace_window_frames,
                     static_cast<double>(trace_zero_direct) / trace_window_frames);
            trace_draws = trace_skipped = trace_indirect = trace_zero_direct = 0;
            trace_window_frames = 0;
            trace_capture_dropped = trace_failure_details = 0;
            trace_rate.next = now + std::chrono::seconds{1};
        }
        ++trace_frame;
        trace_captures = 0;
        trace_sample_frame = trace_frame < 4 || trace_rate.Ready(now);
    }
    ComputePipeline::TickUpscalerTrace();
    draw_counter = 0;
    pending_native_work = false;
    pipeline_cache.TickFrame();
    guest_descriptor_queue.TickFrame();
    compute_pass_descriptor_queue.TickFrame();
    descriptor_buffer_ring.TickFrame();
    fence_manager.TickFrame();
    staging_pool.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.TickFrame();
    }
}

VideoCommon::ConditionalRendering RasterizerVulkan::AccelerateConditionalRendering() {
    {
        VideoCommon::Nx2Profile::Scope flush_scope{
            VideoCommon::Nx2Profile::Zone::RenderEnableFlush};
        gpu_memory->FlushCaching();
    }
    return query_cache.AccelerateHostConditionalRendering();
}

bool RasterizerVulkan::HasDrawTransformFeedback() {
    return device.IsTransformFeedbackDrawSupported();
}

bool RasterizerVulkan::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                             const Tegra::Engines::Fermi2D::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

bool RasterizerVulkan::UsesNvidiaDlss() const {
    return texture_cache_runtime.UsesNvidiaDlss();
}

bool RasterizerVulkan::SupportsGameDlssNetwork() const {
    // Compute pipelines are created 32 threads wide where the device can be told to.
    return device.IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT);
}

std::optional<VideoCore::GameDlssProbe> RasterizerVulkan::ProbeGameDlssOutput(
    const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.ProbeGameDlssOutput(dst, src);
}

bool RasterizerVulkan::AccelerateTemporalUpscale(
    const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst,
    const Tegra::Engines::Fermi2D::Surface& motion_vectors, const Tegra::Texture::TICEntry* depth,
    std::array<f32, 2> jitter, std::array<f32, 2> motion_scale, bool use_history) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.TemporalUpscale(dst, src, motion_vectors, depth, jitter, motion_scale,
                                          use_history);
}

bool RasterizerVulkan::AccelerateOotDlss(const Tegra::Engines::Fermi2D::Surface& src,
                                         const Tegra::Engines::Fermi2D::Surface& dst,
                                         const Tegra::Texture::TICEntry& depth,
                                         std::array<f32, 2> jitter,
                                         const std::array<f32, 16>& reproject, bool flip_y,
                                         bool reset) {
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.OotDlss(dst, src, depth, jitter, reproject, flip_y, reset, false,
                                 oot_effects_rt ? &*oot_effects_rt : nullptr,
                                 oot_fog_rt ? &*oot_fog_rt : nullptr);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerVulkan::AccessAccelerateDMA() {
    return accelerate_dma;
}

void RasterizerVulkan::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                                std::span<const u8> memory) {
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::InlineToMemory};
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (gpu_memory->UsesNativeTiling()) {
        u32 first_word{};
        if (memory.size() >= sizeof(first_word)) std::memcpy(&first_word, memory.data(), sizeof(first_word));
        VideoCommon::TraceNativeQuery(11, gpu_memory->GetID(), address, cpu_addr.value_or(0),
            0, first_word, static_cast<u32>(copy_size));
        static const GPUVAddr watch = [] {
            const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
            return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
        }();
        if (watch != 0 && address < watch + 16 && address + copy_size > watch) {
            LOG_INFO(Render_Vulkan, "Semaphore watch: inline write {:#x} size {} first word {:#x}",
                     address, copy_size, first_word);
        }
    }
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    // Most writes land within one 4 KiB page of a page written just before. While the mappings
    // don't change (map serial), a page inside the mapped runs is written where GpuToCpuAddress
    // points, as the page walk would; sparse pages still take the walk.
    bool written = false;
    if (copy_size != 0 && (address & Core::DEVICE_PAGEMASK) + copy_size <= Core::DEVICE_PAGESIZE &&
        (address + copy_size <= Tegra::g_nx2_sparse_low.load(std::memory_order_relaxed) ||
         address >= Tegra::g_nx2_sparse_high.load(std::memory_order_relaxed))) {
        struct MappedPage {
            const Tegra::MemoryManager* memory{};
            size_t id{};
            GPUVAddr page{~GPUVAddr{0}};
            u64 serial{};
            bool mapped{};
        };
        thread_local MappedPage last{};
        const GPUVAddr page = address & ~static_cast<GPUVAddr>(Core::DEVICE_PAGEMASK);
        const u64 serial = gpu_memory->MapSerial();
        if (last.memory != gpu_memory || last.id != gpu_memory->GetID() || last.page != page ||
            last.serial != serial) {
            last = MappedPage{
                .memory = gpu_memory,
                .id = gpu_memory->GetID(),
                .page = page,
                .serial = serial,
                .mapped = gpu_memory->MaxContinuousRange(page, Core::DEVICE_PAGESIZE) >=
                          Core::DEVICE_PAGESIZE,
            };
        }
        if (last.mapped) {
            if (u8* const pointer = device_memory.GetPointer<u8>(*cpu_addr)) {
                std::memcpy(pointer, memory.data(), copy_size);
                written = true;
            }
        }
    }
    VideoCommon::Nx2BufCache::Note(written ? VideoCommon::Nx2BufCache::Stat::InlineFast
                                           : VideoCommon::Nx2BufCache::Stat::InlineSlow);
    if (!written) {
        gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    }
    // EDEN_NX2_INLINE_DEFER=0: invalidate every cache at once (for comparisons).
    static const bool defer = [] {
        const char* value = std::getenv("EDEN_NX2_INLINE_DEFER");
        return value == nullptr || value[0] != '0';
    }();
    // One cache at a time: holding both while waiting for either (std::lock) waited longer, the
    // fencing thread and CPU write traps holding one of them most of the time.
    bool uploaded = false;
    // A run of inline writes into one page: the first finds the page free of GPU-written data
    // under the buffer cache lock, the rest skip the lock while no GPU write was marked since
    // and the page holds no command list. Only this thread marks GPU writes. The skipped
    // Nx2NoteListHostWrite acts only on command list pages, so the page's own bit is enough.
    struct CleanPage {
        const void* cache{};
        DAddr page{~DAddr{0}};
        u64 serial{};
    };
    thread_local CleanPage clean{};
    const DAddr device_page = *cpu_addr & ~static_cast<DAddr>(Core::DEVICE_PAGEMASK);
    const bool one_page =
        copy_size != 0 && (*cpu_addr & Core::DEVICE_PAGEMASK) + copy_size <= Core::DEVICE_PAGESIZE;
    const bool known_clean =
        defer && one_page && clean.cache == &buffer_cache && clean.page == device_page &&
        clean.serial == buffer_cache.gpu_modified_serial.load(std::memory_order_relaxed) &&
        !Tegra::Nx2IsCommandListPage(device_page);
    if (!known_clean) {
        std::scoped_lock lock{buffer_cache.mutex};
        // Guest memory has the words now: a command list copy of the page no longer does.
        buffer_cache.Nx2NoteListHostWrite(*cpu_addr, copy_size);
        // For a write within one page InlineMemory checks exactly that page for GPU-written
        // data: its answer sets the memo, without a second lookup.
        bool page_clean = false;
        uploaded = buffer_cache.InlineMemory(*cpu_addr, copy_size, memory, &page_clean);
        if (!uploaded && !defer) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
        clean.cache = nullptr;
        if (!uploaded && one_page && page_clean) {
            clean = CleanPage{
                .cache = &buffer_cache,
                .page = device_page,
                .serial = buffer_cache.gpu_modified_serial.load(std::memory_order_relaxed),
            };
        }
    }
    // Pending query results must not land over the new words later: drop them now.
    query_cache.InvalidateRegion(*cpu_addr, copy_size);
    if (!uploaded && defer) {
        // Title 1 makes thousands of small inline writes between draws (~10% of the GPU
        // thread invalidating every cache for each). Guest memory is written; the buffer,
        // texture and shader caches see the merged ranges at the next FlushCaching, which runs
        // before anything of theirs is used. Writes into GPU-written buffer data were uploaded
        // in place above, as before.
        gpu_memory->AccumulateInvalidation(address, copy_size);
        return;
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(*cpu_addr, copy_size);
    }
    pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
}

std::optional<FramebufferTextureInfo> RasterizerVulkan::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr, u32 pixel_stride) {
    if (!framebuffer_addr) {
        return {};
    }
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    static u32 display_trace_count{};
    static const bool frame_capture = std::getenv("EDEN_NATIVE_FRAME_CAPTURE") != nullptr;
    if (frame_capture && display_trace_count++ < 64) {
        LOG_INFO(HW_GPU, "Native display source: address={:#x}, offset={:#x}, dimensions={}x{}, stride={}, accelerated={}, mask={:#x}",
                 framebuffer_addr, config.offset, config.width, config.height, pixel_stride,
                 image_view != nullptr, config.layer_stack_mask);
    }
    if (!image_view) {
        return {};
    }
    if (frame_dump.Capturing()) {
        frame_dump.EndPass();
        frame_dump.CapturePresent(*image_view);
    }

    query_cache.NotifySegment(false);

    const auto& resolution = Settings::values.resolution_info;

    FramebufferTextureInfo info{};
    info.image = image_view->ImageHandle();
    info.image_view = image_view->Handle(Shader::TextureType::Color2D);
    info.width = image_view->size.width;
    info.height = image_view->size.height;
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;
    // Ocarina of Time's scene comes from DLSS or FSR: FXAA or SMAA on top only blurs it.
    // EXPERIMENT (do not merge): EDEN_NX2_EXP_PRESENT_FILTERS=1 keeps FXAA/SMAA and the scaling
    // filter on the upscaled image (the A/B of skipping them).
    const bool keep_filters =
        Common::Nx2Switches::Get(Common::Nx2Switches::Switch::KeepPresentFilters) != 0;
    info.anti_aliased = !keep_filters && VideoCommon::OotDlss::Targets().output_upscaled.load(
                                             std::memory_order_relaxed);
    return info;
}

void RasterizerVulkan::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}

void RasterizerVulkan::FlushWork() {
#ifdef __ANDROID__
    static constexpr bool android = true;
#else
    static constexpr bool android = false;
#endif // __ANDROID__
    const bool native = gpu_memory->UsesNativeTiling();
    u32 budget = SubmissionPolicy::DefaultNativeBudget;
    if (native && !android) {
        static const std::optional<u32> configured_budget = []() -> std::optional<u32> {
            if (const char* value = std::getenv("EDEN_NX2_FLUSH_DRAWS")) {
                if (const auto parsed = SubmissionPolicy::ParseBudget(value)) {
                    return parsed;
                }
                LOG_WARNING(Render_Vulkan,
                            "Invalid EDEN_NX2_FLUSH_DRAWS; expected an integer from 1 to {}",
                            (std::numeric_limits<u32>::max)());
            }
            return std::nullopt;
        }();
        budget = configured_budget.value_or(
            SubmissionPolicy::NativeBudget(device.GetDriverID() == VK_DRIVER_ID_AMD_PROPRIETARY));
    }
    u32 legacy_budget = 4096;
    if (!native && !android) {
        static const u32 forced_draws = [] {
            const char* value = std::getenv("EDEN_NX2_FLUSH_DRAWS");
            return value != nullptr
                       ? static_cast<u32>(std::max(32L, std::strtol(value, nullptr, 10)) & ~31L)
                       : 0U;
        }();
        legacy_budget = forced_draws != 0 ? forced_draws : (device.IsMoltenVK() ? 128U : 4096U);
    }
    switch (SubmissionPolicy::Advance(draw_counter, native, android, budget, legacy_budget,
                                      scheduler.IsRenderPassActive())) {
    case SubmissionPolicy::Action::None:
        break;
    case SubmissionPolicy::Action::Dispatch:
        scheduler.DispatchWork(true);
        break;
    case SubmissionPolicy::Action::Flush:
        scheduler.Flush();
        pending_native_work = false;
        break;
    }
    if (native && !android) {
        pending_native_work = true;
    }
}

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_,
                             Scheduler& scheduler_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, scheduler{scheduler_} {}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex};
    buffer_cache.Nx2CollectListCopies(true);
    const bool copied = buffer_cache.DMACopy(src_address, dest_address, amount);
    if (buffer_cache.Nx2CopyListPages(scheduler.CurrentTick())) {
        scheduler.Flush();
    }
    buffer_cache.Nx2CollectListCopies(false);
    return copied;
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % BytesPerBlock(image->info.format)) {
            return false;
        }
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

void RasterizerVulkan::BuildDynamicStateMask() {
    using DynamicStateDelta::Bit;
    using DynamicStateDelta::Slot;
    dyn_state_mask_built = true;
    auto& mask = dyn_state_mask;
    mask.reset();
    const auto set = [&mask](std::initializer_list<int> ids) {
        for (const int id : ids) {
            mask[static_cast<size_t>(id)] = true;
        }
    };
    u64 slots = Bit(Slot::Viewports) | Bit(Slot::Scissors) | Bit(Slot::DepthBias) |
                Bit(Slot::BlendConstants) | Bit(Slot::DepthBounds) | Bit(Slot::LineWidth) |
                Bit(Slot::StencilReference) | Bit(Slot::StencilWriteMask) |
                Bit(Slot::StencilCompareMask);
    // The flags UpdateDynamicStates consumes (clears) on this device. Flags it doesn't look at
    // here (state enables under StateEnable, stencil parts under StencilProperties) stay out.
    set({Dirty::Viewports, Dirty::Scissors, Dirty::DepthBias, Dirty::BlendConstants,
         Dirty::DepthBounds, Dirty::StencilProperties, Dirty::LineWidth,
         VideoCommon::Dirty::RescaleViewports, VideoCommon::Dirty::RescaleScissors,
         VideoCommon::Dirty::DepthBiasGlobal});
    if (device.IsExtExtendedDynamicStateSupported()) {
        set({Dirty::CullMode, Dirty::DepthCompareOp, Dirty::FrontFace, Dirty::StencilOp,
             Dirty::StateEnable});
        slots |= Bit(Slot::CullMode) | Bit(Slot::DepthCompareOp) | Bit(Slot::FrontFace) |
                 Bit(Slot::StencilOp) | Bit(Slot::DepthBoundsTestEnable) |
                 Bit(Slot::DepthTestEnable) | Bit(Slot::DepthWriteEnable) |
                 Bit(Slot::StencilTestEnable) | Bit(Slot::PrimitiveTopology);
    }
    if (device.IsExtExtendedDynamicState2Supported()) {
        set({Dirty::PrimitiveRestartEnable, Dirty::RasterizerDiscardEnable,
             Dirty::DepthBiasEnable});
        slots |= Bit(Slot::PrimitiveRestartEnable) | Bit(Slot::RasterizerDiscardEnable) |
                 Bit(Slot::DepthBiasEnable);
    }
    if (device.IsExtExtendedDynamicState2ExtrasSupported()) {
        set({Dirty::LogicOp});
        slots |= Bit(Slot::LogicOp);
    }
    if (device.IsExtExtendedDynamicState3EnablesSupported()) {
        set({Dirty::LogicOpEnable, Dirty::DepthClampEnable, Dirty::LineStippleEnable,
             Dirty::ConservativeRasterizationMode, Dirty::AlphaToCoverageEnable,
             Dirty::AlphaToOneEnable});
        if (device.IsExtLineRasterizationSupported()) {
            set({Dirty::LineRasterizationMode});
        }
        slots |= Bit(Slot::LogicOpEnable) | Bit(Slot::DepthClampEnable) |
                 Bit(Slot::LineStippleEnable) | Bit(Slot::ConservativeRasterizationMode) |
                 Bit(Slot::AlphaToCoverageEnable) | Bit(Slot::AlphaToOneEnable) |
                 Bit(Slot::LineRasterizationMode);
        // The AMD logic op workaround (HostLogicOpEnable) follows the vertex formats, which have
        // no dirty flag of their own here: UpdateDynamicStates compares its value instead.
        const auto driver = device.GetDriverID();
        logic_op_float_workaround =
            device.SupportsDynamicState3LogicOpEnable() &&
            (driver == VK_DRIVER_ID_AMD_OPEN_SOURCE || driver == VK_DRIVER_ID_AMD_PROPRIETARY);
    }
    if (device.IsExtExtendedDynamicState3BlendingSupported()) {
        set({Dirty::Blending});
        slots |=
            Bit(Slot::ColorWriteMask) | Bit(Slot::ColorBlendEnable) | Bit(Slot::ColorBlendEquation);
    } else if (device.IsExtColorWriteEnableSupported()) {
        set({Dirty::ColorMask});
        slots |= Bit(Slot::ColorWriteEnable);
    }
    if (device.IsExtVertexInputDynamicStateSupported()) {
        set({Dirty::VertexInput, VideoCommon::Dirty::VertexBuffers});
        slots |= Bit(Slot::VertexInput);
    }
    const bool shader_objects = pipeline_cache.UsesShaderObjects();
    const DynamicFeatures& features = pipeline_cache.Features();
    dyn_polygon_mode = shader_objects || features.has_dynamic_polygon_mode;
    dyn_provoking_vertex = (shader_objects && device.IsExtProvokingVertexSupported()) ||
                           features.has_dynamic_provoking_vertex;
    dyn_line_stipple = (shader_objects && device.IsExtLineRasterizationSupported()) ||
                       features.has_dynamic_line_stipple;
    dyn_patch_control_points =
        shader_objects || features.has_extended_dynamic_state_2_patch_control_points;
    const auto dynamic_raster = [&](bool dynamic, int flag, Slot slot) {
        if (dynamic) {
            set({flag});
            slots |= Bit(slot);
        }
    };
    dynamic_raster(dyn_polygon_mode, Dirty::PolygonMode, Slot::PolygonMode);
    dynamic_raster(dyn_provoking_vertex, Dirty::ProvokingVertex, Slot::ProvokingVertexMode);
    dynamic_raster(dyn_line_stipple, Dirty::LineStippleParams, Slot::LineStipple);
    dynamic_raster(dyn_patch_control_points, Dirty::PatchControlPoints, Slot::PatchControlPoints);
    if (shader_objects) {
        // A pipeline's static state, set per draw from the bound pipeline's key.
        slots |= SHADER_OBJECT_SLOTS;
    }
    dyn_state_slots = slots;
    // EXPERIMENT (do not merge): EDEN_NX2_DYNSTATE_BATCH=0 records each changed state as its own
    // command, the recording before the per-draw packet, for the A/B.
    // Read per draw (DynStateBatched), so the debug server's `switch` command flips it.
    LOG_INFO(Render_Vulkan, "Dynamic state: {} (EDEN_NX2_DYNSTATE_BATCH)",
             DynStateBatched() ? "one packet per draw" : "one command per changed state");
    dyn_apply_options = {
        .with_count = pipeline_cache.UsesShaderObjects(),
        .depth_bias_force_unorm = device.IsExtDepthBiasControlSupported(),
    };
}

u32 RasterizerVulkan::ComputeDynamicState(DynamicStateDelta::Slot slot,
                                          const Tegra::Engines::Maxwell3D::Regs& regs,
                                          const GraphicsPipeline* pipeline, u32* out) {
    using DynamicStateDelta::FloatWord;
    using DynamicStateDelta::Slot;
    const auto one = [out](u32 value) {
        out[0] = value;
        return 1U;
    };
    const auto two = [out](u32 first, u32 second) {
        out[0] = first;
        out[1] = second;
        return 2U;
    };
    const auto current_pipeline = [&]() -> const GraphicsPipeline* {
        return pipeline != nullptr ? pipeline : pipeline_cache.CurrentGraphicsPipeline();
    };
    const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
    // Shader objects: the state a pipeline would hold, from the bound pipeline's key.
    const FixedPipelineState* const so_state =
        pipeline != nullptr && pipeline->UsesShaderObjects() ? &pipeline->State() : nullptr;
    switch (slot) {
    case Slot::Viewports: {
        const float scale =
            texture_cache.IsRescaling() ? Settings::values.resolution_info.up_factor : 1.0f;
        const auto put = [out](u32 index, const VkViewport& viewport) {
            u32* const v = out + 1 + index * 6;
            v[0] = FloatWord(viewport.x);
            v[1] = FloatWord(viewport.y);
            v[2] = FloatWord(viewport.width);
            v[3] = FloatWord(viewport.height);
            v[4] = FloatWord(viewport.minDepth);
            v[5] = FloatWord(viewport.maxDepth);
        };
        out[0] = num_viewports;
        if (!regs.viewport_scale_offset_enabled) {
            float x = static_cast<float>(regs.surface_clip.x);
            float y = static_cast<float>(regs.surface_clip.y);
            float width = (std::max)(1.0f, static_cast<float>(regs.surface_clip.width));
            float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
            if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
                y += height;
                height = -height;
            }
            const VkViewport viewport{
                .x = x,
                .y = y,
                .width = width,
                .height = height,
                .minDepth = 0.0f,
                .maxDepth = 1.0f,
            };
            for (u32 index = 0; index < num_viewports; ++index) {
                put(index, viewport);
            }
        } else {
            // Ocarina of Time's jitter shifts the whole raster by a sub-pixel amount (scaled
            // pixels when rescaling).
            const std::array<float, 2> jitter = OotDlssViewportJitter(regs);
            for (u32 index = 0; index < num_viewports; ++index) {
                VkViewport viewport = GetViewportState(device, regs, index, scale);
                viewport.x += jitter[0];
                viewport.y += jitter[1];
                put(index, viewport);
            }
        }
        return 1 + num_viewports * 6;
    }
    case Slot::Scissors: {
        const auto put = [out](u32 index, const VkRect2D& scissor) {
            u32* const s = out + 1 + index * 4;
            s[0] = static_cast<u32>(scissor.offset.x);
            s[1] = static_cast<u32>(scissor.offset.y);
            s[2] = scissor.extent.width;
            s[3] = scissor.extent.height;
        };
        out[0] = num_viewports;
        if (!regs.viewport_scale_offset_enabled) {
            const u32 x = regs.surface_clip.x;
            u32 y = regs.surface_clip.y;
            const u32 width = (std::max)(1u, static_cast<u32>(regs.surface_clip.width));
            const u32 height = (std::max)(1u, static_cast<u32>(regs.surface_clip.height));
            if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
                y = regs.surface_clip.height - (y + height);
            }
            VkRect2D scissor{};
            scissor.offset.x = static_cast<int32_t>(x);
            scissor.offset.y = static_cast<int32_t>(y);
            scissor.extent.width = width;
            scissor.extent.height = height;
            for (u32 index = 0; index < num_viewports; ++index) {
                put(index, scissor);
            }
        } else {
            const bool rescaling = texture_cache.IsRescaling();
            const u32 up_scale = rescaling ? Settings::values.resolution_info.up_scale : 1U;
            const u32 down_shift = rescaling ? Settings::values.resolution_info.down_shift : 0U;
            for (u32 index = 0; index < num_viewports; ++index) {
                put(index, GetScissorState(regs, index, up_scale, down_shift));
            }
        }
        return 1 + num_viewports * 4;
    }
    case Slot::DepthBias: {
        float units = regs.depth_bias / 2.0f;
        const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                            regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                            regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                            regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;
        if (is_d24 && !device.SupportsD24DepthBuffer()) {
            static constexpr const size_t length = sizeof(NEEDS_D24) / sizeof(NEEDS_D24[0]);
            static constexpr const u64* start = NEEDS_D24;
            static constexpr const u64* end = NEEDS_D24 + length;
            if (std::find(start, end, program_id) != end) {
                // the base formulas can be obtained from here:
                //   https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias
                const double rescale_factor =
                    static_cast<double>(1ULL << (32 - 24)) / (static_cast<double>(0x1.ep+127));
                units = static_cast<float>(static_cast<double>(units) * rescale_factor);
            }
        }
        out[0] = FloatWord(units);
        out[1] = FloatWord(regs.depth_bias_clamp);
        out[2] = FloatWord(regs.slope_scale_depth_bias);
        return 3;
    }
    case Slot::BlendConstants:
        out[0] = FloatWord(regs.blend_color.r);
        out[1] = FloatWord(regs.blend_color.g);
        out[2] = FloatWord(regs.blend_color.b);
        out[3] = FloatWord(regs.blend_color.a);
        return 4;
    case Slot::DepthBounds:
        return two(FloatWord(regs.depth_bounds[0]), FloatWord(regs.depth_bounds[1]));
    case Slot::LineWidth:
        return one(FloatWord(regs.line_anti_alias_enable ? regs.line_width_smooth
                                                         : regs.line_width_aliased));
    case Slot::StencilReference:
        return two(regs.stencil_front_ref,
                   regs.stencil_two_side_enable ? regs.stencil_back_ref : regs.stencil_front_ref);
    case Slot::StencilWriteMask:
        return two(regs.stencil_front_mask,
                   regs.stencil_two_side_enable ? regs.stencil_back_mask : regs.stencil_front_mask);
    case Slot::StencilCompareMask:
        return two(regs.stencil_front_func_mask, regs.stencil_two_side_enable
                                                     ? regs.stencil_back_func_mask
                                                     : regs.stencil_front_func_mask);
    case Slot::CullMode:
        return one(regs.gl_cull_test_enabled
                       ? static_cast<u32>(MaxwellToVK::CullFace(regs.gl_cull_face))
                       : static_cast<u32>(VK_CULL_MODE_NONE));
    case Slot::FrontFace: {
        VkFrontFace front_face = MaxwellToVK::FrontFace(regs.gl_front_face);
        if (regs.window_origin.flip_y != 0) {
            front_face = front_face == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                               : VK_FRONT_FACE_CLOCKWISE;
        }
        return one(static_cast<u32>(front_face));
    }
    case Slot::DepthTestEnable:
        return one(regs.depth_test_enable != 0 ? 1U : 0U);
    case Slot::DepthWriteEnable:
        return one(regs.depth_write_enabled != 0 ? 1U : 0U);
    case Slot::DepthCompareOp:
        return one(static_cast<u32>(MaxwellToVK::ComparisonOp(regs.depth_test_func)));
    case Slot::DepthBoundsTestEnable:
        return one(regs.depth_bounds_enable != 0 && device.IsDepthBoundsSupported() ? 1U : 0U);
    case Slot::StencilTestEnable:
        return one(regs.stencil_enable != 0 ? 1U : 0U);
    case Slot::StencilOp: {
        const auto op = [](Maxwell::StencilOp::Op value) {
            return static_cast<u32>(MaxwellToVK::StencilOp(value));
        };
        const auto compare = [](Maxwell::ComparisonOp value) {
            return static_cast<u32>(MaxwellToVK::ComparisonOp(value));
        };
        const bool two_sided = regs.stencil_two_side_enable != 0;
        const auto& front = regs.stencil_front_op;
        const auto& back = regs.stencil_back_op;
        out[0] = two_sided ? 1U : 0U;
        out[1] = op(front.fail);
        out[2] = op(front.zpass);
        out[3] = op(front.zfail);
        out[4] = compare(front.func);
        out[5] = two_sided ? op(back.fail) : 0U;
        out[6] = two_sided ? op(back.zpass) : 0U;
        out[7] = two_sided ? op(back.zfail) : 0U;
        out[8] = two_sided ? compare(back.func) : 0U;
        return 9;
    }
    case Slot::PrimitiveTopology:
        return one(static_cast<u32>(
            MaxwellToVK::PrimitiveTopology(device, maxwell3d->draw_manager.draw_state.topology)));
    case Slot::PrimitiveRestartEnable: {
        bool enable = regs.primitive_restart.enabled != 0;
        if (device.IsMoltenVK()) {
            enable = true;
        } else if (enable) {
            enable = IsPrimitiveRestartSupported(
                device, MaxwellToVK::PrimitiveTopology(
                            device, maxwell3d->draw_manager.draw_state.topology));
        }
        return one(enable ? 1U : 0U);
    }
    case Slot::RasterizerDiscardEnable:
        return one(regs.rasterize_enable == 0 ? 1U : 0U);
    case Slot::DepthBiasEnable: {
        constexpr size_t POINT = 0;
        constexpr size_t LINE = 1;
        constexpr size_t POLYGON = 2;
        static constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
            POINT,   // Points
            LINE,    // Lines
            LINE,    // LineLoop
            LINE,    // LineStrip
            POLYGON, // Triangles
            POLYGON, // TriangleStrip
            POLYGON, // TriangleFan
            POLYGON, // Quads
            POLYGON, // QuadStrip
            POLYGON, // Polygon
            LINE,    // LinesAdjacency
            LINE,    // LineStripAdjacency
            POLYGON, // TrianglesAdjacency
            POLYGON, // TriangleStripAdjacency
            POLYGON, // Patches
        };
        const std::array enabled_lut{
            regs.polygon_offset_point_enable,
            regs.polygon_offset_line_enable,
            regs.polygon_offset_fill_enable,
        };
        const u32 topology_index = u32(maxwell3d->draw_manager.draw_state.topology);
        return one(enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]] != 0 ? 1U : 0U);
    }
    case Slot::LogicOp: {
        const auto op_value = static_cast<u32>(regs.logic_op.op);
        return one(op_value >= 0x1500 && op_value < 0x1510 ? op_value - 0x1500
                                                           : static_cast<u32>(VK_LOGIC_OP_NO_OP));
    }
    case Slot::LogicOpEnable:
        if (!device.SupportsDynamicState3LogicOpEnable()) {
            return 0;
        }
        return one(VideoCommon::HostLogicOpEnable(regs, logic_op_float_workaround) ? 1U : 0U);
    case Slot::DepthClampEnable: {
        if (!device.SupportsDynamicState3DepthClampEnable()) {
            return 0;
        }
        using GeometryClip = Maxwell::ViewportClipControl::GeometryClip;
        const GeometryClip clip = regs.viewport_clip_control.geometry_clip;
        return one(clip == GeometryClip::Passthrough || clip == GeometryClip::FrustumXYZ ||
                           clip == GeometryClip::FrustumZ
                       ? 0U
                       : 1U);
    }
    case Slot::LineRasterizationMode: {
        if (!device.IsExtLineRasterizationSupported()) {
            return 0;
        }
        if (!device.SupportsDynamicState3LineRasterizationMode()) {
            if (pipeline_cache.UsesShaderObjects()) {
                // What a pipeline without line state rasterizes with.
                return one(static_cast<u32>(VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT));
            }
            static std::once_flag warn_missing_rect;
            std::call_once(warn_missing_rect, [] {
                LOG_WARNING(Render_Vulkan,
                            "Driver lacks rectangular line rasterization support; skipping dynamic "
                            "line state updates");
            });
            return 0;
        }
        VkLineRasterizationModeEXT mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
        if (regs.line_anti_alias_enable != 0) {
            if (device.SupportsSmoothLines()) {
                mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
            } else {
                static std::once_flag warn_missing_smooth;
                std::call_once(warn_missing_smooth, [] {
                    LOG_WARNING(Render_Vulkan,
                                "Line anti-aliasing requested but smoothLines feature "
                                "unavailable; using rectangular rasterization");
                });
            }
        }
        return one(static_cast<u32>(mode));
    }
    case Slot::LineStippleEnable:
        if (!device.SupportsDynamicState3LineStippleEnable()) {
            // Shader objects: off, as pipelines without stippled rectangular lines have it.
            return pipeline_cache.UsesShaderObjects() ? one(0) : 0;
        }
        return one(regs.line_stipple_enable != 0 ? 1U : 0U);
    case Slot::ConservativeRasterizationMode:
        if (!device.SupportsDynamicState3ConservativeRasterizationMode()) {
            return 0;
        }
        return one(static_cast<u32>(regs.conservative_raster_enable != 0
                                        ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                                        : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT));
    case Slot::AlphaToCoverageEnable: {
        if (!device.SupportsDynamicState3AlphaToCoverageEnable()) {
            return 0;
        }
        const GraphicsPipeline* const gp = current_pipeline();
        return one(gp != nullptr && gp->SupportsAlphaToCoverage() &&
                           regs.anti_alias_alpha_control.alpha_to_coverage != 0
                       ? 1U
                       : 0U);
    }
    case Slot::AlphaToOneEnable: {
        // Shader objects set it whenever the device has alpha to one at all.
        if (!device.SupportsDynamicState3AlphaToOneEnable() &&
            !(so_state != nullptr && device.SupportsAlphaToOne())) {
            static std::once_flag warn_alpha_to_one;
            std::call_once(warn_alpha_to_one, [] {
                LOG_WARNING(Render_Vulkan,
                            "Alpha-to-one is not supported on this device; forcing it disabled");
            });
            return 0;
        }
        const GraphicsPipeline* const gp = current_pipeline();
        return one(gp != nullptr && gp->SupportsAlphaToOne() &&
                           regs.anti_alias_alpha_control.alpha_to_one != 0
                       ? 1U
                       : 0U);
    }
    case Slot::ColorWriteMask: {
        // The write mask also excludes components the fragment shader doesn't output.
        const u32 color_output_mask = pipeline ? pipeline->ColorOutputMask() : ~0U;
        u32 masks = 0;
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
            u32 current = 0;
            current |= mask.R ? VK_COLOR_COMPONENT_R_BIT : 0;
            current |= mask.G ? VK_COLOR_COMPONENT_G_BIT : 0;
            current |= mask.B ? VK_COLOR_COMPONENT_B_BIT : 0;
            current |= mask.A ? VK_COLOR_COMPONENT_A_BIT : 0;
            current &= (color_output_mask >> (index * 4)) & 0xF;
            masks |= current << (index * 4);
        }
        return one(masks);
    }
    case Slot::ColorBlendEnable: {
        u32 enables = 0;
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            bool is_integer = false;
            if (regs.rt[index].format != Tegra::RenderTargetFormat::NONE) {
                const auto format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[index].format);
                is_integer = IsPixelFormatInteger(format);
            }
            enables |= (!is_integer && regs.blend.enable[index] != 0) ? 1U << index : 0U;
        }
        return one(enables);
    }
    case Slot::ColorBlendEquation: {
        const auto blend_setup = [](const auto& guest_blend) {
            return DynamicStateDelta::PackBlendEquation({
                .srcColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_source),
                .dstColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_dest),
                .colorBlendOp = MaxwellToVK::BlendEquation(guest_blend.color_op),
                .srcAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_source),
                .dstAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_dest),
                .alphaBlendOp = MaxwellToVK::BlendEquation(guest_blend.alpha_op),
            });
        };
        if (!regs.blend_per_target_enabled) {
            // Single blend equation for all targets
            u32 equation;
            // Temporary workaround for games that use iterated blending
            if (regs.iterated_blend.enable && Settings::values.use_squashed_iterated_blend) {
                equation = DynamicStateDelta::PackBlendEquation({
                    .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
                    .colorBlendOp = VK_BLEND_OP_ADD,
                    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                    .alphaBlendOp = VK_BLEND_OP_ADD,
                });
            } else {
                equation = blend_setup(regs.blend);
            }
            std::fill_n(out, Maxwell::NumRenderTargets, equation);
        } else {
            for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
                out[index] = blend_setup(regs.blend_per_target[index]);
            }
        }
        return Maxwell::NumRenderTargets;
    }
    case Slot::ColorWriteEnable: {
        if (device.IsExtExtendedDynamicState3BlendingSupported()) {
            // Shader objects with the blend state dynamic: every target writes, as in the
            // pipelines (no color write state); the write mask masks.
            if (!so_state || !device.IsExtColorWriteEnableSupported()) {
                return 0;
            }
            return one(0xFFU);
        }
        u32 enables = 0;
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
            enables |= (mask.R || mask.G || mask.B || mask.A) ? 1U << index : 0U;
        }
        return one(enables);
    }
    case Slot::VertexInput: {
        const auto it = vertex_input_layouts.find(MakeVertexInputKey(regs));
        if (it == vertex_input_layouts.end()) {
            return 0;
        }
        const auto words = DynamicStateDelta::VertexInputWords(it->second.get());
        return two(words[0], words[1]);
    }
    case Slot::PolygonMode:
        if (!dyn_polygon_mode) {
            return 0;
        }
        return one(static_cast<u32>(MaxwellToVK::PolygonMode(regs.polygon_mode_front)));
    case Slot::RasterizationSamples:
        if (!so_state) {
            return 0;
        }
        return one(static_cast<u32>(MaxwellToVK::MsaaMode(so_state->msaa_mode)));
    case Slot::SampleMask:
        // Pipelines have no sample mask: every sample.
        if (!so_state) {
            return 0;
        }
        return two(static_cast<u32>(MaxwellToVK::MsaaMode(so_state->msaa_mode)), ~0U);
    case Slot::TessellationDomainOrigin:
        // Pipelines take the default (no domain origin state).
        if (!so_state) {
            return 0;
        }
        return one(static_cast<u32>(VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT));
    case Slot::PatchControlPoints:
        if (!dyn_patch_control_points) {
            return 0;
        }
        return one(std::min(std::clamp<u32>(regs.patch_vertices, 1, 32),
                            device.GetMaxTessellationPatchSize()));
    case Slot::DepthClipEnable:
        // VK_EXT_depth_clip_enable is not enabled: depth clip follows depth clamp.
        return 0;
    case Slot::DepthClipNegativeOneToOne:
        if (!so_state || !device.IsExtDepthClipControlSupported()) {
            return 0;
        }
        return one(so_state->ndc_minus_one_to_one.Value() != 0 ? 1U : 0U);
    case Slot::ProvokingVertexMode:
        if (!dyn_provoking_vertex) {
            return 0;
        }
        // UseLastProvokingVertex settles the device's modes, as the pipeline key does.
        return one(static_cast<u32>(
            FixedPipelineState::UseLastProvokingVertex(regs, pipeline_cache.Features())
                ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT));
    case Slot::LineStipple:
        if (!dyn_line_stipple) {
            return 0;
        }
        return two(regs.line_stipple_params.factor, regs.line_stipple_params.pattern);
    case Slot::ExtraPrimitiveOverestimationSize:
        if (!so_state || !device.IsExtConservativeRasterizationSupported()) {
            return 0;
        }
        return one(FloatWord(0.0f));
    case Slot::RasterizationStream:
        if (!so_state || !device.IsExtTransformFeedbackSupported() ||
            !device.AreTransformFeedbackGeometryStreamsSupported()) {
            return 0;
        }
        return one(0);
    case Slot::Count:
        break;
    }
    UNREACHABLE();
    return 0;
}

void RasterizerVulkan::StageDynamicState(DynamicStateDelta::Slot slot,
                                         const Tegra::Engines::Maxwell3D::Regs& regs,
                                         const GraphicsPipeline* pipeline) {
    std::array<u32, DynamicStateDelta::MaxWords(DynamicStateDelta::Slot::Viewports)> words;
    const u32 count = ComputeDynamicState(slot, regs, pipeline, words.data());
    if (count == 0) {
        return;
    }
    if (dyn_state.Stage(slot, std::span<const u32>(words.data(), count)) && !DynStateBatched()) {
        // One command per changed state.
        RecordDynamicStatePacket(DynamicStateDelta::Bit(slot));
    }
}

void RasterizerVulkan::RecordDynamicStatePacket(u64 slots) {
    std::array<u32, DynamicStateDelta::MAX_PACKET_WORDS> packet;
    const size_t size = dyn_state.TakeSlots(slots, packet);
    if (size == 0) {
        return;
    }
    // Sized to the packet: most draws change a few words.
    const auto record = [&]<size_t N>(std::integral_constant<size_t, N>) {
        std::array<u32, N> words;
        std::copy_n(packet.data(), size, words.data());
        scheduler.Record([words, size, options = dyn_apply_options](vk::CommandBuffer cmdbuf) {
            DynamicStateDelta::ApplyPacket(std::span<const u32>(words.data(), size), cmdbuf,
                                           options);
        });
    };
    if (size <= 8) {
        record(std::integral_constant<size_t, 8>{});
    } else if (size <= 24) {
        record(std::integral_constant<size_t, 24>{});
    } else if (size <= 64) {
        record(std::integral_constant<size_t, 64>{});
    } else if (size <= 128) {
        record(std::integral_constant<size_t, 128>{});
    } else {
        record(std::integral_constant<size_t, DynamicStateDelta::MAX_PACKET_WORDS>{});
    }
}

void RasterizerVulkan::VerifyDynamicState(const Tegra::Engines::Maxwell3D::Regs& regs,
                                          const GraphicsPipeline* pipeline, bool emulated_mesh) {
    using DynamicStateDelta::Bit;
    using DynamicStateDelta::Slot;
    namespace NativeVerify = VideoCommon::NativeVerify;
    u64 slots = dyn_state_slots;
    if (emulated_mesh) {
        // The emulation pipeline's static input assembly and vertex input.
        slots &= ~(Bit(Slot::PrimitiveTopology) | Bit(Slot::PrimitiveRestartEnable) |
                   Bit(Slot::VertexInput));
    }
    const GraphicsPipeline* const gp =
        pipeline != nullptr ? pipeline : pipeline_cache.CurrentGraphicsPipeline();
    if (gp != nullptr && !gp->UsesExtendedDynamicState()) {
        // The pipeline's static state applies.
        return;
    }
    if (gp == nullptr || !gp->HasDynamicVertexInput()) {
        slots &= ~Bit(Slot::VertexInput);
    }
    std::array<u32, DynamicStateDelta::MaxWords(Slot::Viewports)> words;
    for (u64 rest = slots; rest != 0; rest &= rest - 1) {
        const auto slot = static_cast<Slot>(std::countr_zero(rest));
        const u32 count = ComputeDynamicState(slot, regs, pipeline, words.data());
        if (count == 0) {
            continue;
        }
        const std::span<const u32> value(words.data(), count);
        const std::span<const u32> held = dyn_state.Held(slot);
        const auto head = [](std::span<const u32> span) {
            return fmt::format("{:08x}",
                               fmt::join(span.first(std::min<size_t>(span.size(), 8)), " "));
        };
        NativeVerify::Check(NativeVerify::Layer::DynState, dyn_state.Holds(slot, value),
                            "slot {} holds {} ({} words), the registers give {} ({} words)",
                            static_cast<u32>(slot), held.empty() ? "nothing" : head(held),
                            held.size(), head(value), value.size());
    }
    NativeVerify::Report();
}

void RasterizerVulkan::UpdateDynamicStates(const GraphicsPipeline* pipeline) {
    using DynamicStateDelta::Bit;
    using DynamicStateDelta::Slot;
    auto& regs = maxwell3d->regs;
    auto& flags = maxwell3d->dirty.flags;
    if (!dyn_state_mask_built) {
        BuildDynamicStateMask();
    }
    dyn_state.SetEpoch(state_tracker.CommandBufferEpoch());
    // A mesh emulation pipeline has static input assembly and vertex input. Binding it replaces
    // that dynamic state, so leave it to the next regular draw to set again.
    const bool emulated_mesh{pipeline != nullptr && pipeline->IsEmulatedMesh()};
    if (emulated_mesh) {
        state_tracker.InvalidateInputState();
        dyn_state.Forget(Bit(Slot::PrimitiveTopology) | Bit(Slot::PrimitiveRestartEnable) |
                         Bit(Slot::VertexInput));
    }
    const auto topology = maxwell3d->draw_manager.draw_state.topology;
    const bool topology_changed = !emulated_mesh && state_tracker.ChangePrimitiveTopology(topology);
    // The color write mask also follows the pipeline's fragment outputs.
    const u32 color_output_mask = pipeline ? pipeline->ColorOutputMask() : ~0U;
    const bool color_output_changed = device.IsExtExtendedDynamicState3BlendingSupported() &&
                                      color_output_mask != last_color_output_mask;
    // The AMD logic op workaround's value moves with the vertex formats, without a flag.
    const bool logic_op_changed = logic_op_float_workaround && LogicOpEnableChanged(regs);
    // Alpha to coverage/one follow the pipeline's fragment outputs, the blend enables the
    // render target formats (integer targets don't blend); neither raises their flags.
    // With both registers off the value is off whatever the pipeline (and turning one off
    // raises its flag).
    const bool pipeline_changed = device.IsExtExtendedDynamicState3EnablesSupported() &&
                                  pipeline != last_dyn_pipeline &&
                                  (regs.anti_alias_alpha_control.alpha_to_coverage != 0 ||
                                   regs.anti_alias_alpha_control.alpha_to_one != 0);
    bool rt_formats_changed = false;
    if (device.IsExtExtendedDynamicState3BlendingSupported()) {
        for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
            rt_formats_changed |= regs.rt[index].format != last_rt_formats[index];
        }
    }
    // Shader objects: the state of the bound pipeline's key, again when the pipeline changes or
    // the command buffer no longer holds it.
    // HLE macros write transform_feedback_enabled without a dirty flag.
    const bool xfb_changed =
        dyn_provoking_vertex && regs.transform_feedback_enabled != last_xfb_enabled;
    const bool so_changed =
        pipeline != nullptr && pipeline->UsesShaderObjects() &&
        (pipeline != last_so_pipeline || dyn_state.Held(Slot::RasterizationSamples).empty());
    const bool verify =
        VideoCommon::NativeVerify::Enabled(VideoCommon::NativeVerify::Layer::DynState);
    // Ocarina of Time's sub-pixel jitter moves the viewports with no register written (a new
    // frame, a draw into or away from the scene depth): the viewport slot is staged then.
    const bool oot_jitter_changed = OotDlssViewportJitter(regs) != applied_viewport_jitter;
    // Nothing dirty: every slot below holds its value already.
    if (!topology_changed && !logic_op_changed && !color_output_changed && !pipeline_changed &&
        !rt_formats_changed && !oot_jitter_changed && !so_changed && !xfb_changed &&
        !(flags & dyn_state_mask).any()) {
        if (verify) {
            VerifyDynamicState(regs, pipeline, emulated_mesh);
        }
        return;
    }
    if (topology_changed) {
        flags[Dirty::DepthBiasEnable] = true;
        flags[Dirty::PrimitiveRestartEnable] = true;
    }
    last_dyn_pipeline = pipeline;
    const auto stage = [&](Slot slot) { StageDynamicState(slot, regs, pipeline); };
    if (so_changed) {
        last_so_pipeline = pipeline;
        for (u64 rest = SHADER_OBJECT_SLOTS; rest != 0; rest &= rest - 1) {
            stage(static_cast<Slot>(std::countr_zero(rest)));
        }
    }

    if (state_tracker.TouchViewports() || oot_jitter_changed) {
        flags[Dirty::Scissors] = true;
        NoteOotViewportJitter(regs);
        stage(Slot::Viewports);
    }
    if (state_tracker.TouchScissors()) {
        stage(Slot::Scissors);
    }
    if (state_tracker.TouchDepthBias()) {
        stage(Slot::DepthBias);
    }
    if (state_tracker.TouchBlendConstants()) {
        stage(Slot::BlendConstants);
    }
    if (state_tracker.TouchDepthBounds()) {
        stage(Slot::DepthBounds);
    }
    if (state_tracker.TouchStencilProperties()) {
        // The parts' flags only say which part moved; the values are compared anyway.
        (void)state_tracker.TouchStencilReference();
        (void)state_tracker.TouchStencilWriteMask();
        (void)state_tracker.TouchStencilCompare();
        stage(Slot::StencilReference);
        stage(Slot::StencilWriteMask);
        stage(Slot::StencilCompareMask);
    }
    if (state_tracker.TouchLineWidth()) {
        stage(Slot::LineWidth);
    }

    if (device.IsExtExtendedDynamicStateSupported()) {
        if (state_tracker.TouchCullMode()) {
            stage(Slot::CullMode);
        }
        if (state_tracker.TouchDepthCompareOp()) {
            stage(Slot::DepthCompareOp);
        }
        if (state_tracker.TouchFrontFace()) {
            stage(Slot::FrontFace);
        }
        if (state_tracker.TouchStencilOp()) {
            stage(Slot::StencilOp);
        }
        if (state_tracker.TouchStateEnable()) {
            if (state_tracker.TouchDepthBoundsTestEnable()) {
                stage(Slot::DepthBoundsTestEnable);
            }
            if (state_tracker.TouchDepthTestEnable()) {
                stage(Slot::DepthTestEnable);
            }
            if (state_tracker.TouchDepthWriteEnable()) {
                stage(Slot::DepthWriteEnable);
            }
            if (state_tracker.TouchStencilTestEnable()) {
                stage(Slot::StencilTestEnable);
            }
        }
        if (topology_changed) {
            stage(Slot::PrimitiveTopology);
        }
    }

    if (device.IsExtExtendedDynamicState2Supported()) {
        if (!emulated_mesh && state_tracker.TouchPrimitiveRestartEnable()) {
            stage(Slot::PrimitiveRestartEnable);
        }
        if (state_tracker.TouchRasterizerDiscardEnable()) {
            stage(Slot::RasterizerDiscardEnable);
        }
        if (state_tracker.TouchDepthBiasEnable()) {
            stage(Slot::DepthBiasEnable);
        }
    }

    if (device.IsExtExtendedDynamicState2ExtrasSupported() && state_tracker.TouchLogicOp()) {
        stage(Slot::LogicOp);
    }

    if (device.IsExtExtendedDynamicState3EnablesSupported()) {
        // With the AMD workaround the value also follows the vertex formats.
        if (state_tracker.TouchLogicOpEnable() || logic_op_float_workaround) {
            stage(Slot::LogicOpEnable);
        }
        if (state_tracker.TouchDepthClampEnable()) {
            stage(Slot::DepthClampEnable);
        }
        if (device.IsExtLineRasterizationSupported() &&
            state_tracker.TouchLineRasterizationMode()) {
            stage(Slot::LineRasterizationMode);
        }
        if (state_tracker.TouchLineStippleEnable()) {
            stage(Slot::LineStippleEnable);
        }
        if (state_tracker.TouchConservativeRasterizationMode()) {
            stage(Slot::ConservativeRasterizationMode);
        }
        if (state_tracker.TouchAlphaToCoverageEnable() || pipeline_changed) {
            stage(Slot::AlphaToCoverageEnable);
        }
        if (state_tracker.TouchAlphaToOneEnable() || pipeline_changed) {
            stage(Slot::AlphaToOneEnable);
        }
    }

    if (dyn_polygon_mode && state_tracker.TouchPolygonMode()) {
        stage(Slot::PolygonMode);
    }
    if (dyn_provoking_vertex && (state_tracker.TouchProvokingVertex() || xfb_changed)) {
        last_xfb_enabled = regs.transform_feedback_enabled;
        stage(Slot::ProvokingVertexMode);
    }
    if (dyn_line_stipple && state_tracker.TouchLineStipple()) {
        stage(Slot::LineStipple);
    }
    if (dyn_patch_control_points && state_tracker.TouchPatchControlPoints()) {
        stage(Slot::PatchControlPoints);
    }

    if (device.IsExtExtendedDynamicState3BlendingSupported()) {
        if (state_tracker.TouchBlending() || color_output_changed || rt_formats_changed) {
            if (state_tracker.TouchColorMask() || color_output_changed) {
                last_color_output_mask = color_output_mask;
                stage(Slot::ColorWriteMask);
            }
            if (state_tracker.TouchBlendEnable() || rt_formats_changed) {
                for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
                    last_rt_formats[index] = regs.rt[index].format;
                }
                stage(Slot::ColorBlendEnable);
            }
            if (state_tracker.TouchBlendEquations()) {
                stage(Slot::ColorBlendEquation);
            }
        }
    } else if (device.IsExtColorWriteEnableSupported() && state_tracker.TouchColorMask()) {
        stage(Slot::ColorWriteEnable);
    }

    if (device.IsExtVertexInputDynamicStateSupported() && !emulated_mesh) {
        // A draw passes the pipeline it looked up: a second lookup was ~2.5% of the back thread.
        const GraphicsPipeline* const gp =
            pipeline != nullptr ? pipeline : pipeline_cache.CurrentGraphicsPipeline();
        if (gp && gp->HasDynamicVertexInput()) {
            UpdateVertexInput(regs);
        }
    }
    RecordDynamicStatePacket(~u64{0});
    if (verify) {
        VerifyDynamicState(regs, pipeline, emulated_mesh);
    }
}

void RasterizerVulkan::UpdateViewportsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    // The jitter depends on the bound depth target and the frame, which the viewport dirty flag
    // does not track: re-send the viewports whenever it changes.
    const bool jitter_changed = OotDlssViewportJitter(regs) != applied_viewport_jitter;
    if (!state_tracker.TouchViewports() && !jitter_changed) {
        return;
    }
    NoteOotViewportJitter(regs);
    if (!dyn_state_mask_built) {
        BuildDynamicStateMask();
    }
    maxwell3d->dirty.flags[Dirty::Scissors] = true;
    dyn_state.SetEpoch(state_tracker.CommandBufferEpoch());
    StageDynamicState(DynamicStateDelta::Slot::Viewports, regs, nullptr);
    RecordDynamicStatePacket(DynamicStateDelta::Bit(DynamicStateDelta::Slot::Viewports));
}

bool RasterizerVulkan::LogicOpEnableChanged(const Tegra::Engines::Maxwell3D::Regs& regs) const {
    const u32 enable = VideoCommon::HostLogicOpEnable(regs, logic_op_float_workaround) ? 1U : 0U;
    // A new command buffer holds no value: Holds is false then.
    return !dyn_state.Holds(DynamicStateDelta::Slot::LogicOpEnable,
                            std::span<const u32>(&enable, 1));
}

void RasterizerVulkan::HandleTransformFeedback() {
    static std::once_flag warn_unsupported;

    const auto& regs = maxwell3d->regs;
    if (!device.IsExtTransformFeedbackSupported()) {
        if (regs.transform_feedback_enabled != 0) {
            std::call_once(warn_unsupported, [&] {
                LOG_WARNING(Render_Vulkan, "Transform feedback requested by guest but VK_EXT_transform_feedback is unavailable; queries disabled");
            });
        } else {
            std::call_once(warn_unsupported, [&] {
                LOG_INFO(Render_Vulkan, "VK_EXT_transform_feedback not available on device");
            });
        }
        return;
    }
    query_cache.CounterEnable(VideoCommon::QueryType::StreamingByteCount,
                              regs.transform_feedback_enabled);
    if (regs.transform_feedback_enabled != 0) {
        // Log extension usage for transform feedback
        if (GPU::Logging::IsActive()) {
            GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
                "VK_EXT_transform_feedback", "HandleTransformFeedback");
        }
        UNIMPLEMENTED_IF(regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
                         regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation));
    }
}

void RasterizerVulkan::CaptureOotCamera() {
    using namespace VideoCommon::OotDlss;
    const auto& regs = maxwell3d->regs;
    const u64 depth = Targets().depth_gpu.load(std::memory_order_relaxed);
    if (depth == 0 || !regs.zeta_enable || regs.zeta.Address() != depth) {
        return;
    }
    Camera& camera = CurrentCamera();
    const u64 frame = SceneFrame().load(std::memory_order_relaxed);
    std::scoped_lock lock{camera.mutex};
    if (camera.frame != frame) {
        // Last frame's view-projection is this frame's previous one.
        camera.has_last = camera.found && camera.frame + 1 == frame;
        camera.last_view_projection = camera.view_projection;
        camera.frame = frame;
        camera.valid = false;
        camera.found = false;
        camera.moving = false;
        camera.searched = 0;
    }
    if (camera.found) {
        return;
    }
    // Camera motion is this frame's view-projection against last frame's, as captured here. The
    // game's own "previous" matrices don't follow the frames drawn: on a pan their motion matched
    // the static scenery's object motion in direction only once the pair was swapped, and not in
    // length. Which of a pair is this frame's also depends on the shader.
    const auto distance = [](const float* a, const float* b) {
        float difference = 0.0f, magnitude = 0.0f;
        for (int i = 0; i < 16; ++i) {
            difference += std::abs(a[i] - b[i]);
            magnitude += std::abs(a[i]);
        }
        return std::pair{difference, magnitude};
    };
    static std::pair<u32, u32> logged{~0u, ~0u};
    // Of the camera's pair (`low` before `high` in the buffer), whether `high` is this frame's:
    // it is (seen: c[7] +0x100), unless it is clearly the one last frame's capture is close to.
    // The older matrix need not be last frame's, so "further from last frame's" alone can't
    // tell them apart.
    // With a still camera both match last frame's up to float noise, which then picked the other
    // one every frame: a back-and-forth motion of ~0.1 px while standing still (indoors). There
    // the slot taken last frame (`prefer_high`) stays.
    const auto high_is_current = [&](const float* low, const float* high, bool prefer_high) {
        if (!camera.has_last) {
            return prefer_high;
        }
        const auto [to_low, magnitude] = distance(low, camera.last_view_projection.data());
        const float to_high = distance(high, camera.last_view_projection.data()).first;
        if ((std::max)(to_low, to_high) < 1e-4f * magnitude) {
            return prefer_high;
        }
        return !(to_high < 0.5f * to_low);
    };
    const auto take = [&](u32 index, u32 offset, u32 partner, const float* current) {
        if (camera.has_last) {
            // Another camera (a different view drawn into the scene depth) is not this one.
            const auto [difference, magnitude] =
                distance(current, camera.last_view_projection.data());
            if (difference > 0.3f * magnitude) {
                return false;
            }
        }
        std::array<float, 16> view_projection{};
        std::copy_n(current, 16, view_projection.begin());
        const auto inverse = Inverse(view_projection);
        if (!inverse) {
            return false;
        }
        const auto& previous = camera.has_last ? camera.last_view_projection : view_projection;
        const auto reproject = Multiply(previous, *inverse);
        if (!std::ranges::all_of(reproject, [](float v) { return std::isfinite(v); })) {
            return false;
        }
        camera.reproject = reproject;
        camera.view_projection = view_projection;
        camera.valid = true;
        camera.found = true;
        camera.moving = previous != view_projection;
        camera.known_index = index;
        camera.known_offset = offset;
        camera.known_partner = partner;
        // EDEN_OOT_CAMERA_TRACE=1: per frame, the capture and the screen centre's camera motion.
        static const bool trace = [] {
            const char* value = std::getenv("EDEN_OOT_CAMERA_TRACE");
            return value != nullptr && value[0] == '1';
        }();
        if (trace) {
            constexpr float z = 0.9f;
            const float w = reproject[14] * z + reproject[15];
            std::array<float, 16> other{};
            gpu_memory->ReadBlockUnsafe(
                maxwell3d->state.shader_stages[0].const_buffers[index].address + partner,
                other.data(), 64);
            const float to_last =
                camera.has_last ? distance(current, camera.last_view_projection.data()).first
                                : -1.0f;
            const float partner_to_last =
                camera.has_last ? distance(other.data(), camera.last_view_projection.data()).first
                                : -1.0f;
            // How much each slot changed since last frame's capture (a still view: the slot the
            // game renders with stays the same).
            static std::unordered_map<u32, std::array<float, 16>> slot_last;
            const auto changed = [&](u32 slot, const float* matrix) {
                std::array<float, 16> value{};
                std::copy_n(matrix, 16, value.begin());
                const auto it = slot_last.find(slot);
                const float result =
                    it != slot_last.end() ? distance(matrix, it->second.data()).first : -1.0f;
                slot_last[slot] = value;
                return result;
            };
            const float own_change = changed(offset, current);
            const float partner_change = changed(partner, other.data());
            // The scene constants' view-projection pair (c[3] +0x30 / +0x180), for comparison.
            float c3_change = -1.0f;
            float c3_pair = -1.0f;
            if (const auto& c3 = maxwell3d->state.shader_stages[0].const_buffers[3];
                c3.enabled && c3.size >= 0x1c0 &&
                gpu_memory->IsFullyMappedRange(c3.address, 0x1c0)) {
                std::array<float, 16> a{};
                std::array<float, 16> b{};
                gpu_memory->ReadBlockUnsafe(c3.address + 0x30, a.data(), 64);
                gpu_memory->ReadBlockUnsafe(c3.address + 0x180, b.data(), 64);
                c3_pair = distance(a.data(), b.data()).first;
                c3_change = changed(0x10000 | 0x30, a.data());
            }
            LOG_INFO(Render_Vulkan, "OoT DLSS camera trace: c[3]+0x30 change {:.4f}, to +0x180 {:.4f}",
                     c3_change, c3_pair);
            // Every perspective matrix the vertex stage sees, with its change since last frame.
            std::string all;
            for (u32 slot = 0; slot < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& cbuf = maxwell3d->state.shader_stages[0].const_buffers[slot];
                const u32 size = cbuf.enabled ? (std::min)(cbuf.size, 0x800u) & ~15u : 0;
                if (size < 64 || !gpu_memory->IsFullyMappedRange(cbuf.address, size)) {
                    continue;
                }
                std::array<float, 0x200> words{};
                gpu_memory->ReadBlockUnsafe(cbuf.address, words.data(), size);
                for (u32 at = 0; at + 64 <= size; at += 16) {
                    if (IsPerspective(words.data() + at / 4)) {
                        all += fmt::format(" c[{}]+{:#x}:{:.4f}", slot, at,
                                           changed(0x20000 | (slot << 12) | at,
                                                   words.data() + at / 4));
                    }
                }
            }
            LOG_INFO(Render_Vulkan, "OoT DLSS camera trace: all{}", all);
            LOG_INFO(Render_Vulkan,
                     "OoT DLSS camera trace: frame {} c[{}] {:#x} ({:.4f} from last; partner {:#x} "
                     "{:.4f}), slot changes {:.4f} / {:.4f}, centre motion {:.5f} {:.5f} NDC",
                     frame, index, offset, to_last, partner, partner_to_last, own_change,
                     partner_change, w != 0.0f ? (reproject[2] * z + reproject[3]) / w : 0.0f,
                     w != 0.0f ? (reproject[6] * z + reproject[7]) / w : 0.0f);
        }
        static u32 changes{};
        if (std::pair{index, offset} != logged && changes < 32) {
            logged = {index, offset};
            ++changes;
            LOG_INFO(Render_Vulkan, "OoT DLSS camera: c[{}] view-projection at {:#x}", index,
                     offset);
        }
        return true;
    };
    // Where the camera's pair was found last: checked on every scene draw, since only some
    // shaders bind it.
    if (camera.known_index != ~0u) {
        const auto& cbuf = maxwell3d->state.shader_stages[0].const_buffers[camera.known_index];
        const u32 low_offset = (std::min)(camera.known_offset, camera.known_partner);
        const u32 high_offset = (std::max)(camera.known_offset, camera.known_partner);
        if (cbuf.enabled && cbuf.size >= high_offset + 64 &&
            gpu_memory->IsFullyMappedRange(cbuf.address, high_offset + 64)) {
            std::array<float, 16> low{};
            std::array<float, 16> high{};
            gpu_memory->ReadBlockUnsafe(cbuf.address + low_offset, low.data(), 64);
            gpu_memory->ReadBlockUnsafe(cbuf.address + high_offset, high.data(), 64);
            if (IsPerspective(low.data()) && IsPerspective(high.data())) {
                const bool high_current = high_is_current(low.data(), high.data(),
                                                           camera.known_offset == high_offset);
                if (take(camera.known_index, high_current ? high_offset : low_offset,
                         high_current ? low_offset : high_offset,
                         high_current ? high.data() : low.data())) {
                    return;
                }
            }
        }
    }
    if (camera.searched >= 64) {
        return;
    }
    ++camera.searched;
    // The scene draws' vertex constant buffers hold pairs of close perspective matrices at
    // offsets that differ between shaders. The camera's pair holds this frame's view-projection
    // and an older one (c[7] +0xc0 / +0x100; this frame's is the one further from last frame's
    // capture, else the second). Some buffers hold one matrix twice (c[3] +0xa0 / +0x270) that
    // stays the same on a pan: it only stands in, with no camera motion, until the camera is
    // found, and is never remembered.
    constexpr u32 MaxBytes = 0x800;
    std::array<float, MaxBytes / 4> words{};
    for (u32 slot = 0; slot < Tegra::Engines::Maxwell3D::Regs::MaxConstBuffers; ++slot) {
        const u32 index = slot == 0 ? 3 : (slot == 3 ? 0 : slot);
        const auto& cbuf = maxwell3d->state.shader_stages[0].const_buffers[index];
        const u32 size = cbuf.enabled ? (std::min)(cbuf.size, MaxBytes) & ~15u : 0;
        if (size < 128 || !gpu_memory->IsFullyMappedRange(cbuf.address, size)) {
            continue;
        }
        gpu_memory->ReadBlockUnsafe(cbuf.address, words.data(), size);
        boost::container::small_vector<u32, 8> candidates;
        for (u32 offset = 0; offset + 64 <= size; offset += 16) {
            if (IsPerspective(words.data() + offset / 4)) {
                candidates.push_back(offset);
            }
        }
        for (size_t a = 0; a < candidates.size(); ++a) {
            for (size_t b = a + 1; b < candidates.size(); ++b) {
                if (candidates[b] < candidates[a] + 64) {
                    continue; // overlapping rows of one matrix
                }
                const float* first = words.data() + candidates[a] / 4;
                const float* second = words.data() + candidates[b] / 4;
                const auto [difference, magnitude] = distance(first, second);
                if (difference > 0.3f * magnitude) {
                    continue;
                }
                if (difference == 0.0f) {
                    if (!camera.valid) {
                        // No camera motion, until the camera is found.
                        std::copy_n(first, 16, camera.view_projection.begin());
                        const auto inverse = Inverse(camera.view_projection);
                        if (inverse) {
                            camera.reproject = Multiply(camera.view_projection, *inverse);
                            camera.valid = true;
                        }
                    }
                    continue;
                }
                const u32 offset =
                    high_is_current(first, second,
                                    !(camera.known_index == index &&
                                      camera.known_offset == candidates[a]))
                        ? candidates[b]
                        : candidates[a];
                const u32 partner = offset == candidates[a] ? candidates[b] : candidates[a];
                if (take(index, offset, partner, words.data() + offset / 4)) {
                    return;
                }
            }
        }
    }
}

void RasterizerVulkan::OotRedrawTonemap(bool is_indexed, u32 instance_count) {
    using namespace VideoCommon::OotDlss;
    using Tegra::Engines::Fermi2D;
    auto& targets = Targets();
    const u64 tonemapped = targets.tonemapped_gpu.load(std::memory_order_relaxed);
    const u32 layout = targets.tonemapped_layout.load(std::memory_order_relaxed);
    if (tonemapped == 0 || (layout & (3u << 12)) != 0) {
        return;
    }
    Fermi2D::Surface image{};
    image.format = Tegra::RenderTargetFormat::B10G11R11_FLOAT;
    image.linear = Fermi2D::MemoryLayout::BlockLinear;
    image.block_width.Assign(layout & 0xf);
    image.block_height.Assign((layout >> 4) & 0xf);
    image.block_depth.Assign((layout >> 8) & 0xf);
    image.depth = 1;
    image.width = targets.tonemapped_width.load(std::memory_order_relaxed);
    image.height = targets.tonemapped_height.load(std::memory_order_relaxed);
    image.addr_upper = static_cast<u32>(tonemapped >> 32);
    image.addr_lower = static_cast<u32>(tonemapped);
    {
        std::scoped_lock lock{texture_cache.mutex};
        if (!texture_cache.OotSnapshotScene(image, 2)) {
            NoteFallback("tonemapped image not copied");
            return;
        }
    }
    if (!OotSnapshotScene(0, true)) {
        return;
    }
    oot_redrawing = true;
    Draw(is_indexed, instance_count);
    oot_redrawing = false;
    OotSnapshotScene(1, true);
    targets.particle_frame.store(SceneFrame().load(std::memory_order_relaxed),
                                 std::memory_order_relaxed);
}

bool RasterizerVulkan::OotSnapshotScene(u32 which, bool restore) {
    using namespace VideoCommon::OotDlss;
    using Tegra::Engines::Fermi2D;
    auto& targets = Targets();
    const u64 colour = targets.color_gpu.load(std::memory_order_relaxed);
    std::optional<Tegra::Texture::TICEntry> output_tic;
    {
        std::scoped_lock lock{targets.depth_mutex};
        output_tic = targets.output_tic;
    }
    if (colour == 0 || !output_tic) {
        return false;
    }
    // The scene colour has the network output's size, format and layout.
    Fermi2D::Surface scene{};
    scene.format = Tegra::RenderTargetFormat::B10G11R11_FLOAT;
    scene.linear = Fermi2D::MemoryLayout::BlockLinear;
    scene.block_width.Assign(output_tic->block_width);
    scene.block_height.Assign(output_tic->block_height);
    scene.block_depth.Assign(output_tic->block_depth);
    scene.depth = 1;
    scene.width = output_tic->Width();
    scene.height = output_tic->Height();
    scene.addr_upper = static_cast<u32>(colour >> 32);
    scene.addr_lower = static_cast<u32>(colour);
    const u64 frame = SceneFrame().load(std::memory_order_relaxed);
    std::scoped_lock lock{texture_cache.mutex};
    if (!texture_cache.OotSnapshotScene(scene, which, restore)) {
        NoteFallback("scene colour not copied");
        return false;
    }
    if (which == 0 && !restore) {
        targets.particle_base_frame.store(frame, std::memory_order_relaxed);
    }
    return true;
}

void RasterizerVulkan::OotDlssBeforeTonemap() {
    using namespace VideoCommon::OotDlss;
    using Tegra::Engines::Fermi2D;
    auto& targets = Targets();
    const u64 scene_frame = SceneFrame().load(std::memory_order_relaxed);
    if (targets.hdr_frame.load(std::memory_order_relaxed) == scene_frame) {
        return;
    }
    const u64 colour = targets.color_gpu.load(std::memory_order_relaxed);
    std::optional<Tegra::Texture::TICEntry> depth_tic;
    std::optional<Tegra::Texture::TICEntry> output_tic;
    {
        std::scoped_lock lock{targets.depth_mutex};
        depth_tic = targets.depth_tic;
        output_tic = targets.output_tic;
    }
    std::array<float, 16> reproject{};
    bool valid{};
    {
        Camera& camera = CurrentCamera();
        std::scoped_lock lock{camera.mutex};
        valid = camera.valid;
        reproject = camera.reproject;
    }
    if (colour == 0 || !depth_tic || !output_tic || !valid) {
        NoteFallback(colour == 0    ? "scene colour not seen yet"
                     : !depth_tic   ? "scene depth not seen yet"
                     : !output_tic  ? "network output not seen yet"
                                    : "camera not found");
        return;
    }
    // The scene colour has the network output's size, format and layout.
    const auto surface = [&](u64 address) {
        Fermi2D::Surface result{};
        result.format = Tegra::RenderTargetFormat::B10G11R11_FLOAT;
        result.linear = Fermi2D::MemoryLayout::BlockLinear;
        result.block_width.Assign(output_tic->block_width);
        result.block_height.Assign(output_tic->block_height);
        result.block_depth.Assign(output_tic->block_depth);
        result.depth = 1;
        result.width = output_tic->Width();
        result.height = output_tic->Height();
        result.addr_upper = static_cast<u32>(address >> 32);
        result.addr_lower = static_cast<u32>(address);
        return result;
    };
    const Fermi2D::Surface output = surface(output_tic->Address());
    const Fermi2D::Surface scene = surface(colour);
    const s32 width = static_cast<s32>(output_tic->Width());
    const s32 height = static_cast<s32>(output_tic->Height());
    const Fermi2D::Config copy{
        .operation = Fermi2D::Operation::SrcCopy,
        .filter = Fermi2D::Filter::Point,
        .must_accelerate = true,
        .dst_x0 = 0,
        .dst_y0 = 0,
        .dst_x1 = width,
        .dst_y1 = height,
        .src_x0 = 0,
        .src_y0 = 0,
        .src_x1 = width,
        .src_y1 = height,
    };
    std::scoped_lock lock{texture_cache.mutex};
    // DLSS writes the network output's image (free until the network pass), then that goes
    // back over the scene colour the tonemap reads.
    if (!texture_cache.OotDlss(output, scene, *depth_tic, ReportedJitter(scene_frame), reproject, FlipY(),
                               HistoryReset(), true,
                               // The particles are drawn after DLSS; the effects mask only
                               // cost the rest of the effects layer its anti-aliasing.
                               oot_effects_rt && HdrDlssStage() == HdrStage::Tonemap
                                   ? &*oot_effects_rt
                                   : nullptr,
                               oot_fog_rt ? &*oot_fog_rt : nullptr)) {
        return;
    }
    // Marked even if the copy back fails: the network pass must not evaluate DLSS twice.
    targets.hdr_frame.store(scene_frame, std::memory_order_relaxed);
    if (!texture_cache.BlitImage(scene, output, copy)) {
        NoteFallback("HDR result not copied back");
        return;
    }
    static bool logged{};
    if (!std::exchange(logged, true)) {
        LOG_INFO(Render_Vulkan, "OoT DLSS: running on the HDR scene colour {:#x} before the tonemap",
                 colour);
    }
}

void RasterizerVulkan::OotSceneEnd() {
    if (oot_object_motion) {
        oot_object_motion->BeginFrame(scheduler);
    }
}

void RasterizerVulkan::PrepareOotObjectMotion() {
    // The image only changes size in TickFrame: fast draws reuse descriptors within a frame.
    if (!oot_object_motion) {
        oot_object_motion = std::make_unique<OotObjectMotion>(device, memory_allocator);
        oot_object_motion->EnsureImage(scheduler, OotObjectMotionSize());
    }
}

VkExtent2D RasterizerVulkan::OotObjectMotionSize() const {
    const auto& targets = VideoCommon::OotDlss::Targets();
    u32 width = targets.depth_width.load(std::memory_order_relaxed);
    u32 height = targets.depth_height.load(std::memory_order_relaxed);
    if (width == 0 || height == 0) {
        return VkExtent2D{1, 1};
    }
    if (VideoCommon::OotDlss::SceneRescaled()) {
        width = Settings::values.resolution_info.ScaleUp(width);
        height = Settings::values.resolution_info.ScaleUp(height);
    }
    return VkExtent2D{width, height};
}

std::optional<std::pair<u32, u32>> RasterizerVulkan::OotIndexBounds() {
    const auto& index_buffer = maxwell3d->draw_manager.draw_state.index_buffer;
    const auto& restart = maxwell3d->regs.primitive_restart;
    const u64 key = (index_buffer.IndexStart() * 0x9E3779B97F4A7C15ULL) ^
                    (u64{index_buffer.count} << 32) ^
                    (static_cast<u64>(index_buffer.format) << 60) ^
                    (restart.enabled ? u64{restart.index} * 0xC2B2AE3D27D4EB4FULL : 0);
    if (const auto it = oot_index_bounds.find(key); it != oot_index_bounds.end()) {
        return it->second;
    }
    const size_t index_size = index_buffer.FormatSizeInBytes();
    const size_t bytes = size_t{index_buffer.count} * index_size;
    constexpr size_t MaxBytes = size_t{16} << 20;
    if (bytes == 0 || bytes > MaxBytes ||
        !gpu_memory->IsFullyMappedRange(index_buffer.IndexStart(), bytes)) {
        return std::nullopt;
    }
    oot_index_scratch.resize(bytes);
    gpu_memory->ReadBlockUnsafe(index_buffer.IndexStart(), oot_index_scratch.data(), bytes);
    u32 low = ~0u;
    u32 high = 0;
    const u32 restart_index = restart.enabled ? restart.index : ~0u;
    const auto scan = [&](const auto* indices) {
        for (u32 i = 0; i < index_buffer.count; ++i) {
            const u32 value = indices[i];
            if (value == restart_index) {
                continue;
            }
            low = (std::min)(low, value);
            high = (std::max)(high, value);
        }
    };
    switch (index_size) {
    case 1:
        scan(oot_index_scratch.data());
        break;
    case 2:
        scan(reinterpret_cast<const u16*>(oot_index_scratch.data()));
        break;
    default:
        scan(reinterpret_cast<const u32*>(oot_index_scratch.data()));
        break;
    }
    if (low > high) {
        return std::nullopt;
    }
    if (oot_index_bounds.size() > 65536) {
        oot_index_bounds.clear();
    }
    oot_index_bounds.emplace(key, std::make_pair(low, high));
    return std::make_pair(low, high);
}

void RasterizerVulkan::PushOotObjectMotion(u32 base_vertex, u32 num_vertices, u32 num_instances,
                                           u32 base_instance, bool is_indexed, bool enabled) {
    GraphicsPipeline* const pipeline = std::exchange(oot_motion_pipeline, nullptr);
    if (!pipeline) {
        return;
    }
    using Layout = OotObjectMotion::Layout;
    Layout data{.flags = 0};
    const auto& regs = maxwell3d->regs;
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const auto& targets = VideoCommon::OotDlss::Targets();
    const u32 depth_width = targets.depth_width.load(std::memory_order_relaxed);
    const u32 depth_height = targets.depth_height.load(std::memory_order_relaxed);
    const bool scene = depth_width != 0 && regs.zeta_enable && regs.zeta.Address() != 0 &&
                       regs.zeta_size.width == depth_width &&
                       regs.zeta_size.height == depth_height &&
                       regs.zeta.format == Tegra::DepthFormat::Z32_FLOAT &&
                       // Only draws that leave their depth can be told apart from what covers
                       // them (fullscreen fog and lighting passes write no depth).
                       regs.depth_test_enable != 0 && regs.depth_write_enabled != 0;
    const bool quads = draw_state.topology == Maxwell::PrimitiveTopology::Quads ||
                       draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip;
    const VkExtent2D size = OotObjectMotionSize();
    const bool sized = oot_object_motion && oot_object_motion->HasImage() && size.width > 1 &&
                       oot_object_motion->Extent().width == size.width &&
                       oot_object_motion->Extent().height == size.height;
    if (const u32 mesh_vertices = pipeline->OotMeshMaxVertices();
        enabled && scene && sized && mesh_vertices != 0) {
        // Mesh draw: `num_vertices` workgroups, each with its own run of history slots.
        const auto& hashes = pipeline->ShaderHashes();
        u64 identity = 0x84222325cbf29ce4ULL;
        for (const u64 value : {hashes[1], hashes[2], hashes[5], u64{base_vertex},
                                u64{num_vertices}, u64{mesh_vertices}}) {
            identity = (identity ^ value) * 0x100000001b3ULL;
            identity ^= identity >> 31;
        }
        constexpr u32 MaxWorkgroups = 1u << 16;
        if (const u32 words = pipeline->OotMeshShadowWords(); words != 0) {
            // Snapshot of the constant buffer words its shadow evaluation reads, four per slot:
            // this frame's for the next, last frame's for this one. A mesh drawn several times a
            // frame (its constant buffers move every frame) takes last frame's snapshot at the
            // same place in the draw order; the mesh stage drops shadow positions far from the
            // current ones.
            data = oot_object_motion->Allocate(identity, Common::DivCeil(words, 4u), 1, 0, 0,
                                               true);
            data.vertex_range = num_vertices;
            data.image_width = size.width;
        } else if (num_vertices != 0 && num_vertices <= MaxWorkgroups) {
            // Two history entries per vertex (position, signature: EmitOotMotionMeshVertex).
            data = oot_object_motion->Allocate(identity, mesh_vertices * 2, num_vertices, 0, 0);
            data.vertex_range = num_vertices;
            data.image_width = size.width;
        }
    } else if (enabled && scene && !quads && sized) {
        // Vertices per instance: the draw's own for plain draws; for indexed ones the range
        // its indices span (VertexIndex is the index plus the base vertex).
        u64 range = num_vertices;
        u32 vertex_bias = base_vertex;
        if (is_indexed) {
            range = 0;
            if (const auto bounds = OotIndexBounds()) {
                range = u64{bounds->second} - bounds->first + 1;
                vertex_bias = base_vertex + bounds->first;
            }
        }
        constexpr u64 MaxRange = u64{1} << 20;
        if (range != 0 && range <= MaxRange) {
            const auto& hashes = pipeline->ShaderHashes();
            u64 identity = 0xcbf29ce484222325ULL;
            const auto mix = [&identity](u64 value) {
                identity = (identity ^ value) * 0x100000001b3ULL;
                identity ^= identity >> 31;
            };
            mix(hashes[1]);
            mix(hashes[5]);
            mix(is_indexed);
            mix(base_vertex);
            mix(num_vertices);
            mix(num_instances);
            mix(base_instance);
            mix(range);
            if (is_indexed) {
                mix(draw_state.index_buffer.StartAddress());
                mix(static_cast<u64>(draw_state.index_buffer.format));
                mix(draw_state.index_buffer.first);
            }
            for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
                mix(regs.vertex_streams[index].IsEnabled()
                        ? static_cast<u32>(regs.vertex_streams[index].stride)
                        : 0U);
            }
            data = oot_object_motion->Allocate(identity, static_cast<u32>(range), num_instances,
                                               vertex_bias, base_instance);
            data.image_width = size.width;
        }
    }
    scheduler.Record([this, pipeline, data](vk::CommandBuffer cmdbuf) {
        if (scheduler.GraphicsPipelineMissing()) {
            return;
        }
        cmdbuf.PushConstants(pipeline->PipelineLayout(), pipeline->PushConstantStages(),
                             Shader::Backend::SPIRV::OOT_MOTION_OFFSET, sizeof(data), &data);
    });
}

void RasterizerVulkan::NoteOotViewportJitter(const Tegra::Engines::Maxwell3D::Regs& regs) {
    applied_viewport_jitter = OotDlssViewportJitter(regs);
    if (applied_viewport_jitter[0] != 0.0f || applied_viewport_jitter[1] != 0.0f) {
        ++oot_jittered_updates;
    }
}

std::array<float, 2> RasterizerVulkan::OotDlssViewportJitter(
    const Tegra::Engines::Maxwell3D::Regs& regs) const {
    if (!VideoCommon::OotDlss::Requested() ||
        Settings::GetCurrentProgramID() != VideoCommon::OotDlss::ProgramId) {
        return {};
    }
    const auto& targets = VideoCommon::OotDlss::Targets();
    const u32 width = targets.depth_width.load(std::memory_order_relaxed);
    const u32 height = targets.depth_height.load(std::memory_order_relaxed);
    if (width == 0 || !regs.zeta_enable || regs.zeta.Address() == 0 ||
        regs.zeta_size.width != width || regs.zeta_size.height != height ||
        regs.zeta.format != Tegra::DepthFormat::Z32_FLOAT) {
        return {};
    }
    // Only the scene's geometry: full-screen passes (the tonemap) keep the scene depth bound but
    // neither test nor write it. Jittered, they shifted an already jittered image again (twice
    // the jitter DLSS was told) or, after DLSS on the HDR path, shook the finished image.
    if (!regs.depth_test_enable && !regs.depth_write_enabled) {
        return {};
    }
    const u64 frame = VideoCommon::OotDlss::SceneFrame().load(std::memory_order_relaxed);
    if (targets.hdr_frame.load(std::memory_order_relaxed) == frame ||
        targets.particle_base_frame.load(std::memory_order_relaxed) == frame) {
        // DLSS already ran this frame (HdrInput), or the particles are drawing (DLSS takes them
        // from the current frame): nothing after it is jittered.
        return {};
    }
    return VideoCommon::OotDlss::Jitter(frame);
}

size_t RasterizerVulkan::VertexInputKeyHash::operator()(const VertexInputKey& key) const noexcept {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u32 word : key) {
        hash = (hash ^ word) * 0x100000001b3ULL;
    }
    return static_cast<size_t>(hash ^ (hash >> 32));
}

u32 RasterizerVulkan::UpdateVertexRepackLayout(bool dynamic_input) {
    const auto& regs = maxwell3d->regs;
    // VertexRepackMask reads the attribute formats and the strides of their streams; the mask
    // it gave for the same values is still set (nothing else changes it).
    static_assert(Maxwell::NumVertexAttributes == 32 && Maxwell::NumVertexArrays == 32);
    std::array<u32, 64> key;
    for (size_t index = 0; index < 32; ++index) {
        key[index] = regs.vertex_attrib_format[index].hex;
        key[32 + index] = regs.vertex_streams[index].stride.Value();
    }
    if (repack_key_valid && repack_key_dynamic == dynamic_input && key == repack_key) {
        return 0;
    }
    repack_key = key;
    repack_key_dynamic = dynamic_input;
    repack_key_valid = true;
    return buffer_cache_runtime.SetVertexRepackLayout(regs, dynamic_input);
}

void RasterizerVulkan::BuildVertexInputLayout(const Tegra::Engines::Maxwell3D::Regs& regs,
                                              u32 max_attributes, u32 max_bindings,
                                              VertexInputLayout& layout) const {
    auto& bindings = layout.bindings;
    auto& attributes = layout.attributes;
    for (u32 index = 0; index < max_attributes; ++index) {
        const Maxwell::VertexAttribute attribute{regs.vertex_attrib_format[index]};
        const u32 binding{attribute.buffer};
        if (attribute.constant || binding >= max_bindings) {
            continue;
        }
        attributes.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .location = index,
            .binding = binding,
            .format = MaxwellToVK::VertexFormat(device, attribute.type, attribute.size),
            .offset =
                attribute.offset +
                (buffer_cache_runtime.IsVertexRepacked(binding) && attribute.offset >= 65 ? 3 : 0),
        });
    }

    for (u32 binding = 0; binding < max_bindings; ++binding) {
        const auto& input_binding{regs.vertex_streams[binding]};
        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};
        bindings.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .binding = binding,
            .stride =
                buffer_cache_runtime.IsVertexRepacked(binding) ? 72U : input_binding.stride.Value(),
            .inputRate = is_instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = is_instanced ? input_binding.frequency : 1,
        });
    }
}

void RasterizerVulkan::UpdateVertexInput(Tegra::Engines::Maxwell3D::Regs& regs) {
    auto& dirty{maxwell3d->dirty.flags};
    const bool vertex_input_dirty = dirty[Dirty::VertexInput];
    const bool vertex_buffers_dirty = dirty[VideoCommon::Dirty::VertexBuffers];
    if (!vertex_input_dirty && !vertex_buffers_dirty) {
        return;
    }
    dirty[Dirty::VertexInput] = false;

    for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        dirty[Dirty::VertexAttribute0 + index] = false;
    }
    for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        dirty[Dirty::VertexBinding0 + index] = false;
    }

    const VertexInputKey key = MakeVertexInputKey(regs);
    const VertexInputLayout* layout = nullptr;
    if (last_vertex_input_layout != nullptr && key == last_vertex_input_key) {
        layout = last_vertex_input_layout;
    } else if (const auto it = vertex_input_layouts.find(key); it != vertex_input_layouts.end()) {
        layout = it->second.get();
    } else if (vertex_input_layouts.size() < 4096) {
        auto built = std::make_unique<VertexInputLayout>();
        BuildVertexInputLayout(regs, MaxVertexInputAttributes(), MaxVertexInputBindings(), *built);
        layout = vertex_input_layouts.emplace(key, std::move(built)).first->second.get();
    }
    if (layout != nullptr) {
        last_vertex_input_key = key;
        last_vertex_input_layout = layout;
        if (dyn_state.Stage(DynamicStateDelta::Slot::VertexInput,
                            DynamicStateDelta::VertexInputWords(layout)) &&
            !DynStateBatched()) {
            RecordDynamicStatePacket(DynamicStateDelta::Bit(DynamicStateDelta::Slot::VertexInput));
        }
        return;
    }

    // More layouts than are kept: built for this record alone.
    VertexInputLayout built;
    BuildVertexInputLayout(regs, MaxVertexInputAttributes(), MaxVertexInputBindings(), built);
    dyn_state.Forget(DynamicStateDelta::Bit(DynamicStateDelta::Slot::VertexInput));
    scheduler.Record(
        [bindings = built.bindings, attributes = built.attributes](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetVertexInputEXT(bindings, attributes);
        });
}

u32 RasterizerVulkan::MaxVertexInputAttributes() const {
    return static_cast<u32>(
        std::min<size_t>(Maxwell::NumVertexAttributes, device.GetMaxVertexInputAttributes()));
}

u32 RasterizerVulkan::MaxVertexInputBindings() const {
    return static_cast<u32>(
        std::min<size_t>(Maxwell::NumVertexArrays, device.GetMaxVertexInputBindings()));
}

RasterizerVulkan::VertexInputKey RasterizerVulkan::MakeVertexInputKey(
    const Tegra::Engines::Maxwell3D::Regs& regs) const {
    // Everything BuildVertexInputLayout reads; layouts are few (one per vertex format).
    static_assert(Maxwell::NumVertexAttributes <= 32 && Maxwell::NumVertexArrays <= 32);
    const u32 max_attributes = MaxVertexInputAttributes();
    const u32 max_bindings = MaxVertexInputBindings();
    VertexInputKey key{};
    for (u32 index = 0; index < max_attributes; ++index) {
        key[index] = regs.vertex_attrib_format[index].hex;
    }
    u32 repacked = 0;
    for (u32 binding = 0; binding < max_bindings; ++binding) {
        const auto& stream = regs.vertex_streams[binding];
        const bool is_instanced = regs.vertex_stream_instances.IsInstancingEnabled(binding);
        key[32 + binding * 3] = stream.stride.Value();
        key[32 + binding * 3 + 1] = is_instanced ? 1U : 0U;
        key[32 + binding * 3 + 2] = is_instanced ? stream.frequency : 0U;
        repacked |= buffer_cache_runtime.IsVertexRepacked(binding) ? 1U << binding : 0U;
    }
    key[32 + 32 * 3] = repacked;
    return key;
}

void RasterizerVulkan::InitializeChannel(Tegra::Control::ChannelState& channel) {
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
    state_tracker.SetupTables(channel);
    pipeline_cache.ResetStateWatch();
}

void RasterizerVulkan::BindChannel(Tegra::Control::ChannelState& channel) {
    const s32 channel_id = channel.bind_id;
    BindToChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    pipeline_cache.BindToChannel(channel_id);
    query_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
}

void RasterizerVulkan::ReleaseChannel(s32 channel_id) {
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    pipeline_cache.EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}

} // namespace Vulkan
