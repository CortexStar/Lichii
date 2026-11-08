// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <deque>
#include <span>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"

namespace VideoCommon::PoolMirror {

// A bindless descriptor heap mirroring the guest's texture (TIC) and sampler (TSC) pools
// (notes/texture-cache-rework.md, notes/native-gpu-backend.md step 3a). This header holds the
// backend-neutral part: the heap layout shaders index, the per-TIC meta word, sampler classes
// and the copy-on-write versions of heap pages. The texture cache fills it; the Vulkan backend
// writes the descriptors.

/// Logical heap pages: the unit of copy-on-write.
constexpr u32 PAGE_BITS = 12;
constexpr u32 PAGE_SIZE = 1U << PAGE_BITS;
constexpr u32 IMAGE_DESCRIPTOR_SIZE = 32;
constexpr u32 SAMPLER_DESCRIPTOR_SIZE = 16;
constexpr u32 IMAGE_SLOTS_PER_PAGE = PAGE_SIZE / IMAGE_DESCRIPTOR_SIZE;     // 128
constexpr u32 SAMPLER_SLOTS_PER_PAGE = PAGE_SIZE / SAMPLER_DESCRIPTOR_SIZE; // 256
/// Shader::TextureType values: each TIC entry has one image slot per type.
constexpr u32 NUM_IMAGE_TYPES = 9;

/// Per-TIC meta word, read by shaders next to the descriptors.
namespace Meta {
constexpr u32 CLASS_MASK = 0xffff;         ///< Sampler class of the entry's view
constexpr u32 RESCALED = 1U << 16;         ///< The view's image is rescaled (rescaling pass)
constexpr u32 NO_DEPTH_COMPARE = 1U << 17; ///< The view cannot be sampled with a depth compare
constexpr u32 NULL_ENTRY = 1U << 18;       ///< No view: every type reads the null descriptor
/// Integer, ASTC, BCn or ETC2 format: with NO_DEPTH_COMPARE, depth use reads null
/// (IsUncomparable, renderer_vulkan/pipeline_helper.h).
constexpr u32 UNCOMPARABLE_FORMAT = 1U << 19;
} // namespace Meta

/// Strides of the heap regions. Image slot = type * tic_stride + tic. Sampler slot =
/// (class * 2 + depth_use) * tsc_stride + tsc; classes are appended, so the sampler region only
/// grows when a class is added and its existing slots never move.
struct Layout {
    u32 tic_stride{}; ///< TIC limit + 1
    u32 tsc_stride{}; ///< TSC limit + 1
    u32 num_classes{};

    bool operator==(const Layout&) const = default;

    [[nodiscard]] u32 ImageSlot(u32 type, u32 tic) const noexcept {
        return type * tic_stride + tic;
    }

    [[nodiscard]] u32 SamplerSlot(u32 sampler_class, bool depth_use, u32 tsc) const noexcept {
        return (sampler_class * 2 + (depth_use ? 1U : 0U)) * tsc_stride + tsc;
    }

    [[nodiscard]] u32 ImageSlots() const noexcept {
        return NUM_IMAGE_TYPES * tic_stride;
    }

    [[nodiscard]] u32 SamplerSlots() const noexcept {
        return num_classes * 2 * tsc_stride;
    }

    [[nodiscard]] static u32 ImagePage(u32 slot) noexcept {
        return slot / IMAGE_SLOTS_PER_PAGE;
    }

    [[nodiscard]] static u32 SamplerPage(u32 slot) noexcept {
        return slot / SAMPLER_SLOTS_PER_PAGE;
    }
};

/// The view-dependent part of a sampler's host variant (renderer_vulkan Sampler::MakeKey),
/// interned as small class ids. Two views of one class get the same host sampler from one TSC.
class SamplerClasses {
public:
    /// The class of a view key, adding it when new.
    [[nodiscard]] u32 Find(u64 view_key) {
        const auto it = std::ranges::find(keys, view_key);
        if (it != keys.end()) {
            return static_cast<u32>(it - keys.begin());
        }
        ASSERT(keys.size() < Meta::CLASS_MASK);
        keys.push_back(view_key);
        return static_cast<u32>(keys.size() - 1);
    }

    [[nodiscard]] u64 Key(u32 sampler_class) const noexcept {
        return keys[sampler_class];
    }

    [[nodiscard]] u32 Size() const noexcept {
        return static_cast<u32>(keys.size());
    }

    void Clear() noexcept {
        keys.clear();
    }

private:
    std::vector<u64> keys;
};

/**
 * Copy-on-write versions of a heap's pages. Shaders find a logical page's descriptors through a
 * page table (physical = table[logical]); a physical page that a recorded draw may have read is
 * never written again: a write to its logical page goes to a fresh physical page, which the
 * backend fills with a copy of the old one (null descriptors when there is none) before applying
 * the write, and the table changes.
 * The old physical page is recycled once the GPU is past the last command buffer that could
 * read it.
 *
 * The backend reports draws (draw sequence numbers only grow) and command buffer ticks
 * (Scheduler::CurrentTick, IsFree).
 */
class HeapPageVersions {
public:
    static constexpr u32 NO_PAGE = ~0U;

    struct Target {
        u32 physical;  ///< Page to write into
        u32 copy_from; ///< Page to copy whole into it first, NO_PAGE when none
        /// physical was just mapped: it holds whatever its last use left, so it is filled from
        /// copy_from, or with null descriptors when copy_from is NO_PAGE, before the write.
        bool fresh;
    };

    /// Logical pages [0, count): grows the table, new pages unmapped (NO_PAGE).
    void Resize(u32 count) {
        if (count > table.size()) {
            table.resize(count, NO_PAGE);
            table_changed = true;
        }
    }

    /// Drops every mapping (a new pool): every mapped page retires at tick.
    void Reset(u64 tick) {
        for (u32& physical : table) {
            if (physical != NO_PAGE) {
                Retire(physical, tick);
                physical = NO_PAGE;
            }
        }
        table_changed = true;
    }

    /**
     * Where a write to a logical page goes.
     * @param draw_seq Sequence number of the last draw recorded so far (0 before any).
     * @param tick     Tick of the command buffer being recorded.
     * @param is_free  is_free(tick): the GPU is done with that tick.
     */
    template <typename IsFree>
    [[nodiscard]] Target Write(u32 logical, u64 draw_seq, u64 tick, IsFree&& is_free) {
        Resize(logical + 1);
        const u32 current = table[logical];
        if (current != NO_PAGE && pages[current].mapped_after_draw == draw_seq) {
            // No draw was recorded since this page was mapped: nobody has read it.
            return {current, NO_PAGE, false};
        }
        const u32 fresh = Allocate(is_free);
        pages[fresh].mapped_after_draw = draw_seq;
        table[logical] = fresh;
        table_changed = true;
        if (current != NO_PAGE) {
            Retire(current, tick);
        }
        return {fresh, current, true};
    }

    /// The table changed since the last call: the backend makes a new table version.
    [[nodiscard]] bool TakeTableChanged() noexcept {
        return std::exchange(table_changed, false);
    }

    [[nodiscard]] std::span<const u32> Table() const noexcept {
        return table;
    }

    /// Physical page a logical page is in now, NO_PAGE when it was never written (null).
    [[nodiscard]] u32 Map(u32 logical) const noexcept {
        return logical < table.size() ? table[logical] : NO_PAGE;
    }

    [[nodiscard]] u32 NumPhysicalPages() const noexcept {
        return static_cast<u32>(pages.size());
    }

    [[nodiscard]] u32 NumFreePages() const noexcept {
        return static_cast<u32>(free_pages.size());
    }

    /// Whether a physical page is waiting for the GPU before it can be reused.
    [[nodiscard]] bool IsRetired(u32 physical) const noexcept {
        return pages[physical].retired;
    }

private:
    struct Page {
        u64 mapped_after_draw{}; ///< Draw sequence when it was mapped
        u64 retire_tick{};       ///< Tick of the last command buffer that may read it
        bool retired{};
    };

    template <typename IsFree>
    u32 Allocate(IsFree& is_free) {
        // Recycle retired pages the GPU is done with (oldest first).
        while (!retired_pages.empty() && is_free(pages[retired_pages.front()].retire_tick)) {
            free_pages.push_back(retired_pages.front());
            pages[retired_pages.front()].retired = false;
            retired_pages.pop_front();
        }
        if (!free_pages.empty()) {
            const u32 page = free_pages.back();
            free_pages.pop_back();
            return page;
        }
        pages.push_back({});
        return static_cast<u32>(pages.size() - 1);
    }

    void Retire(u32 physical, u64 tick) {
        pages[physical].retire_tick = tick;
        pages[physical].retired = true;
        retired_pages.push_back(physical);
    }

    std::vector<u32> table;
    std::vector<Page> pages;
    std::vector<u32> free_pages;
    std::deque<u32> retired_pages; ///< In retire order, so ticks only grow
    bool table_changed{};
};

/// Which heap a page table or page belongs to.
enum class Heap : u8 {
    Image,
    Sampler,
    Meta, ///< u32 meta words, 1024 per page
};

/// One change for the backend, in the order they are to be applied.
enum class UpdateKind : u8 {
    Reset,   ///< New pool: every slot null, every page table empty; read the layout again
    Layout,  ///< Sampler classes were added: the sampler region grew (read the layout again)
    Table,   ///< A page table of `heap` changed (read it again before the next draw)
    Image,   ///< Image slot `slot` (type `type` of TIC `index`) from `view` (id 0: the null view)
    Meta,    ///< meta[index] = meta
    Sampler, ///< Sampler slot `slot`: `sampler` for views of class key `class_key`, depth use
};

template <typename ViewId, typename SamplerId>
struct Update {
    UpdateKind kind{};
    Heap heap{};
    u8 type{};                                ///< Image: Shader::TextureType value
    bool depth_use{};                         ///< Sampler
    u32 index{};                              ///< TIC (Image, Meta) or TSC (Sampler)
    u32 slot{};                               ///< Logical slot in its heap (Meta: the TIC)
    u32 physical{};                           ///< Physical page the slot is written in
    u32 copy_from{HeapPageVersions::NO_PAGE}; ///< Copy this physical page into it first
    /// `physical` was just mapped: fill it from copy_from (null descriptors when NO_PAGE)
    /// before the write. Not set for writes into a page mapped earlier.
    bool fresh{};
    ViewId view{};
    SamplerId sampler{};
    u32 meta{};
    u64 class_key{}; ///< Sampler: view key of the class
};

constexpr u32 META_WORDS_PER_PAGE = PAGE_SIZE / sizeof(u32);

} // namespace VideoCommon::PoolMirror
