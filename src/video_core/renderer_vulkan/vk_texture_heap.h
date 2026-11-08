// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <memory>

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
/// coherent heap memory never race the GPU. That memory is uncached on the host (write-combined
/// upload memory): every descriptor is built in a cached shadow of the heap and then copied over,
/// so the host never reads the GPU heap (a page copy reads the shadow).
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
    bool PreparePage(VideoCommon::PoolMirror::Heap heap, u32 physical, u32 copy_from,
                     bool reserved = false);

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

    /// EDEN_NX2_NATIVE_VERIFY=bindless: whether the heap holds at `index` exactly the descriptor
    /// WriteImage would write for `view`'s `type` view (or a null descriptor).
    [[nodiscard]] bool HoldsImage(u32 index, const ImageView* view, Shader::TextureType type) const;
    /// Likewise for the sampler descriptor of `sampler`'s `variant`.
    [[nodiscard]] bool HoldsSampler(u32 index, const Sampler& sampler, VkSampler variant) const;

    /// Heap indices of a null image and a default sampler descriptor: each heap's last page is
    /// kept null and never handed to the mirror.
    [[nodiscard]] u32 NullImageIndex() const noexcept {
        return (images.capacity_pages - 1) * VideoCommon::PoolMirror::IMAGE_SLOTS_PER_PAGE;
    }
    [[nodiscard]] u32 NullSamplerIndex() const noexcept {
        return (samplers.capacity_pages - 1) * VideoCommon::PoolMirror::SAMPLER_SLOTS_PER_PAGE;
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
        u8* host{};                   ///< the mapped heap: written only
        std::unique_ptr<u8[]> shadow; ///< the same bytes in cached host memory: read and written
        VkDeviceAddress address{};
        u32 capacity_pages{};
    };

    /// A page or descriptor in the shadow.
    [[nodiscard]] static u8* Shadow(const HeapBuffer& heap, size_t offset) noexcept {
        return heap.shadow.get() + offset;
    }

    /// Copies [offset, offset + size) of the shadow into the heap.
    static void Publish(const HeapBuffer& heap, size_t offset, size_t size) noexcept;

    const Device& device;
    HeapBuffer images;
    HeapBuffer samplers;
    std::array<u8, VideoCommon::PoolMirror::IMAGE_DESCRIPTOR_SIZE> null_image{};
    std::array<u8, VideoCommon::PoolMirror::SAMPLER_DESCRIPTOR_SIZE> null_sampler{};
    bool usable{};
    u64 draw_seq{};
};

} // namespace Vulkan
