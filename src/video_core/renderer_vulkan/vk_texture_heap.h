// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/texture_cache/pool_mirror.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class ImageView;
class MemoryAllocator;
class Sampler;

/// The VK_EXT_descriptor_heap resource and sampler heaps that mirror the guest's TIC and TSC
/// pools (texture_cache/pool_mirror.h). Physical pages are 4 KiB: 128 image descriptors or 256
/// sampler descriptors. The texture cache decides which physical page a write goes to
/// (HeapPageVersions, copy-on-write): a page it hands out is either fresh (no recorded draw can
/// read it) or already current and unread since it was mapped, so host writes into the mapped,
/// coherent heap memory never race the GPU.
class TextureHeap {
public:
    explicit TextureHeap(const Device& device, MemoryAllocator& memory_allocator);
    ~TextureHeap();

    TextureHeap(const TextureHeap&) = delete;
    TextureHeap& operator=(const TextureHeap&) = delete;

    /// Whether the heaps exist: the device has descriptor heaps, null descriptors and the
    /// descriptor sizes the pool mirror lays out (32 B images, 16 B samplers).
    [[nodiscard]] bool Usable() const noexcept {
        return usable;
    }

    /// Before the first write into a physical page the mirror mapped fresh: copy `copy_from`
    /// into it, or fill it with null descriptors when there is nothing to copy. False when the
    /// page is past the heap's capacity (the heap is no longer usable).
    bool PreparePage(VideoCommon::PoolMirror::Heap heap, u32 physical, u32 copy_from);

    /// Image slot `logical_slot` in `physical`: the descriptor of `view`'s `type` view, or a null
    /// descriptor when it has none (as the bound path's null-view fallback).
    void WriteImage(u32 physical, u32 logical_slot, const ImageView* view,
                    Shader::TextureType type);

    /// Sampler slot `logical_slot` in `physical`: the descriptor of `sampler`'s `variant`.
    void WriteSampler(u32 physical, u32 logical_slot, const Sampler& sampler, VkSampler variant);

    /// Applies the mirror's pending changes of the graphics (or compute) pools: prepares the
    /// fresh pages and writes the image and sampler slots. Call it before recording a draw that
    /// reads the heaps, under the texture cache's lock.
    template <typename Cache>
    void ApplyMirrorUpdates(Cache& texture_cache, bool compute) {
        namespace PoolMirror = VideoCommon::PoolMirror;
        texture_cache.TakeMirrorUpdates(compute, draw_seq, [&](const auto& update) {
            switch (update.kind) {
            case PoolMirror::UpdateKind::Image:
                if (update.fresh && !PreparePage(update.heap, update.physical, update.copy_from)) {
                    return;
                }
                WriteImage(update.physical, update.slot,
                           update.view ? &texture_cache.GetImageView(update.view) : nullptr,
                           static_cast<Shader::TextureType>(update.type));
                return;
            case PoolMirror::UpdateKind::Sampler: {
                if (update.fresh && !PreparePage(update.heap, update.physical, update.copy_from)) {
                    return;
                }
                auto& sampler = texture_cache.GetSampler(update.sampler);
                WriteSampler(update.physical, update.slot, sampler,
                             sampler.HandleForClass(update.class_key, update.depth_use));
                return;
            }
            case PoolMirror::UpdateKind::Reset:
            case PoolMirror::UpdateKind::Layout:
            case PoolMirror::UpdateKind::Table:
            case PoolMirror::UpdateKind::Meta:
                // The CPU reads layouts, page tables and meta words from the texture cache when
                // it writes a draw's index tables.
                return;
            }
        });
    }

    /// A draw that reads the heaps was recorded (page versions: a page mapped after it may be
    /// read).
    void NoteDraw() noexcept {
        ++draw_seq;
    }

    [[nodiscard]] VkBindHeapInfoEXT ResourceHeapBinding() const noexcept;
    [[nodiscard]] VkBindHeapInfoEXT SamplerHeapBinding() const noexcept;

    /// Heap index (in descriptors) of a logical slot written in `physical`, as shaders read it.
    [[nodiscard]] static u32 ImageIndex(u32 physical, u32 logical_slot) noexcept {
        return physical * VideoCommon::PoolMirror::IMAGE_SLOTS_PER_PAGE +
               logical_slot % VideoCommon::PoolMirror::IMAGE_SLOTS_PER_PAGE;
    }
    [[nodiscard]] static u32 SamplerIndex(u32 physical, u32 logical_slot) noexcept {
        return physical * VideoCommon::PoolMirror::SAMPLER_SLOTS_PER_PAGE +
               logical_slot % VideoCommon::PoolMirror::SAMPLER_SLOTS_PER_PAGE;
    }

private:
    struct HeapBuffer {
        vk::Buffer buffer;
        u8* host{};
        VkDeviceAddress address{};
        u32 capacity_pages{};
    };

    [[nodiscard]] u8* PageHost(const HeapBuffer& heap, u32 physical) const noexcept;

    const Device& device;
    HeapBuffer images;
    HeapBuffer samplers;
    std::array<u8, VideoCommon::PoolMirror::IMAGE_DESCRIPTOR_SIZE> null_image{};
    std::array<u8, VideoCommon::PoolMirror::SAMPLER_DESCRIPTOR_SIZE> null_sampler{};
    bool usable{};
    u64 draw_seq{};
};

} // namespace Vulkan
