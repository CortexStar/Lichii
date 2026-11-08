// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/assert.h"
#include "core/memory.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

/// A 4 KiB page of a descriptor pool's GPU range. Armed while the texture cache holds a write
/// watch on its device page: a guest or GPU write to it then reaches the cache before the next
/// draw reads the pool (texture-cache-rework.md §3.1).
struct DescriptorPoolPage {
    u64 device_page{}; ///< Device page number of the watch while armed
    u64 armed_frame{};
    u64 retry_frame{}; ///< Not armed again before this frame (back-off after writes)
    u32 strikes{};
    bool armed{};
};

template <typename T, typename Id = u32>
class DescriptorTable {
public:
    static constexpr u32 PAGE_BITS = 12;
    static constexpr u64 PAGE_SIZE = u64{1} << PAGE_BITS;
    /// A page written again within this many frames of being armed backs off longer.
    static constexpr u64 QUIET_FRAMES = 64;
    static constexpr u32 MAX_STRIKES = 6;

    [[nodiscard]] bool NeedsRefresh(GPUVAddr gpu_addr, u32 limit) const noexcept {
        return !(current_gpu_addr == gpu_addr && current_limit == limit);
    }

    [[nodiscard]] bool Synchronize(GPUVAddr gpu_addr, u32 limit) noexcept {
        const bool ret = NeedsRefresh(gpu_addr, limit);
        if (ret) {
            Refresh(gpu_addr, limit);
        }
        return ret;
    }

    void Invalidate() noexcept {
        // Cleared on the next read: the texture cache invalidates every channel's tables for
        // each image it deletes, dozens of times a frame while its collector runs.
        invalidated = true;
    }

    std::span<Id> LiveBoundIds() noexcept {
        return std::span{bound_ids}.first(
            (std::min)(bound_ids.size(), size_t{current_limit} + 1));
    }

    template <class MemoryManager>
    [[nodiscard]] std::pair<T, bool> Read(const MemoryManager& gpu_memory, u32 index) noexcept {
        DEBUG_ASSERT(index <= current_limit);
        if (invalidated) {
            ClearReadDescriptors();
            invalidated = false;
        }
        const GPUVAddr gpu_addr = current_gpu_addr + index * sizeof(T);
        std::pair<T, bool> result;
        // This runs for every texture and sampler of every draw. An entry within one page is
        // contiguous in host memory, so a single page lookup replaces the generic block read.
        const bool one_page =
            (gpu_addr & Core::Memory::YUZU_PAGEMASK) + sizeof(T) <= Core::Memory::YUZU_PAGESIZE;
        if (const u8* const host = one_page ? gpu_memory.GetPointer(gpu_addr) : nullptr) {
            std::memcpy(std::addressof(result.first), host, sizeof(T));
        } else {
            gpu_memory.ReadBlockUnsafe(gpu_addr, std::addressof(result.first), sizeof(T));
        }
        if ((read_descriptors[index / 64] & (1ULL << (index % 64))) != 0) {
            result.second = result.first != descriptors[index];
        } else {
            read_descriptors[index / 64] |= 1ULL << (index % 64);
            result.second = true;
        }
        if (result.second) {
            descriptors[index] = result.first;
        }
        return result;
    }

    /// Rebinds the table. The caller disarms its pages first (ArmedPages() == 0).
    void Refresh(GPUVAddr gpu_addr, u32 limit) noexcept {
        DEBUG_ASSERT(armed_pages == 0);
        current_gpu_addr = gpu_addr;
        current_limit = limit;
        // Mario Brothership reallocates a lot of times, so use aggressive pre-alloc sizes
        // std::vector<T> by default uses quadratic growth, but that isn't even enough to satisfy brothership
        const size_t num_descriptors = ((limit + 0x80000) & (~0x7ffff)) + 1;
        read_descriptors.resize(Common::DivCeil(num_descriptors, 64U));
        resident.resize(read_descriptors.size());
        ClearReadDescriptors();
        invalidated = false;
        //
        descriptors.resize(num_descriptors);
        bound_ids.resize(num_descriptors, Id{0});
        // A new pool: the heap mirroring it starts over, all null.
        mirrored_ids.assign(num_descriptors, Id{0});
        mirror_queued.assign(read_descriptors.size(), u64{0});
        mirror_queue.clear();
        mirror_reset = true;
        const u64 entries = (std::min)(u64{limit} + 1, u64{num_descriptors});
        pages.assign(
            static_cast<size_t>(
                ((gpu_addr & (PAGE_SIZE - 1)) + entries * sizeof(T) + PAGE_SIZE - 1) >> PAGE_BITS),
            DescriptorPoolPage{});
        armed_pages = 0;
    }

    /// The entry's cached descriptor equals guest memory and its bound id needs no new lookup
    /// or preparation (set and cleared by the texture cache's events).
    [[nodiscard]] bool IsResident(u32 index) const noexcept {
        const size_t word = index / 64;
        return !invalidated && index <= current_limit && word < resident.size() &&
               ((resident[word] >> (index % 64)) & 1) != 0;
    }

    void SetResident(u32 index) noexcept {
        if (index <= current_limit && index / 64 < resident.size()) {
            resident[index / 64] |= u64{1} << (index % 64);
        }
    }

    void ClearResident(u32 index) noexcept {
        if (index <= current_limit && index / 64 < resident.size()) {
            resident[index / 64] &= ~(u64{1} << (index % 64));
        }
    }

    void ClearAllResident() noexcept {
        const size_t words = (std::min)(resident.size(), size_t{current_limit} / 64 + 1);
        std::fill_n(resident.begin(), words, u64{0});
    }

    /// The entry's bound object is gone: the next read resolves it again.
    void Forget(u32 index) noexcept {
        if (index > current_limit || index / 64 >= read_descriptors.size()) {
            return;
        }
        const u64 mask = ~(u64{1} << (index % 64));
        read_descriptors[index / 64] &= mask;
        resident[index / 64] &= mask;
        Bind(index, Id{0});
    }

    /// Binds the entry to id and queues it for the heap mirror when the mirror holds another.
    void Bind(u32 index, Id id) noexcept {
        bound_ids[index] = id;
        if (index < mirrored_ids.size() && mirrored_ids[index] != id) {
            u64& word = mirror_queued[index / 64];
            const u64 bit = u64{1} << (index % 64);
            if ((word & bit) == 0) {
                word |= bit;
                mirror_queue.push_back(index);
            }
        }
    }

    /// Whether the heap mirroring this pool has to start over (the table was rebound). Once.
    [[nodiscard]] bool TakeMirrorReset() noexcept {
        return std::exchange(mirror_reset, false);
    }

    /// Calls func(index, id) for every entry whose bound id differs from what the mirror holds,
    /// in the order they changed, and records the mirror as holding it.
    template <typename Func>
    void TakeMirrorUpdates(Func&& func) {
        for (const u32 index : mirror_queue) {
            mirror_queued[index / 64] &= ~(u64{1} << (index % 64));
            const Id id = bound_ids[index];
            if (mirrored_ids[index] != id) {
                mirrored_ids[index] = id;
                func(index, id);
            }
        }
        mirror_queue.clear();
    }

    /// Entries read since the last invalidation whose mirror differs from their bound id
    /// without being queued (0 unless Bind was bypassed). For verify modes and tests.
    [[nodiscard]] size_t MirrorMismatches() const noexcept {
        if (invalidated) {
            return 0;
        }
        size_t mismatches{};
        const size_t entries = (std::min)(mirrored_ids.size(), size_t{current_limit} + 1);
        for (size_t index = 0; index < entries; ++index) {
            const bool read = ((read_descriptors[index / 64] >> (index % 64)) & 1) != 0;
            const bool queued = ((mirror_queued[index / 64] >> (index % 64)) & 1) != 0;
            if (read && !queued && mirrored_ids[index] != bound_ids[index]) {
                ++mismatches;
            }
        }
        return mismatches;
    }

    /// Page of the pool's GPU range holding the entry.
    [[nodiscard]] u32 PageOf(u32 index) const noexcept {
        return static_cast<u32>(((current_gpu_addr & (PAGE_SIZE - 1)) + u64{index} * sizeof(T)) >>
                                PAGE_BITS);
    }

    /// Whether the entry lies within one page (a watch on one page covers all of it).
    [[nodiscard]] bool InOnePage(u32 index) const noexcept {
        const u64 offset = (current_gpu_addr + u64{index} * sizeof(T)) & (PAGE_SIZE - 1);
        return offset + sizeof(T) <= PAGE_SIZE && PageOf(index) < pages.size();
    }

    [[nodiscard]] GPUVAddr PageAddress(u32 page) const noexcept {
        return (current_gpu_addr & ~(PAGE_SIZE - 1)) + u64{page} * PAGE_SIZE;
    }

    /// Whether the page may get a watch now (not armed, not backing off).
    [[nodiscard]] bool MayArm(u32 page, u64 frame) const noexcept {
        return page < pages.size() && !pages[page].armed && frame >= pages[page].retry_frame;
    }

    void NoteArmed(u32 page, u64 device_page, u64 frame) noexcept {
        DescriptorPoolPage& state = pages[page];
        DEBUG_ASSERT(!state.armed);
        state.armed = true;
        state.device_page = device_page;
        state.armed_frame = frame;
        ++armed_pages;
    }

    /// The page's watch ended: its entries are no longer resident. A write backs it off
    /// 2^strikes - 1 frames; a page that stayed unwritten for QUIET_FRAMES starts over.
    void NoteDisarmed(u32 page, u64 frame, bool written) noexcept {
        if (page >= pages.size() || !pages[page].armed) {
            return;
        }
        DescriptorPoolPage& state = pages[page];
        state.armed = false;
        --armed_pages;
        ClearPageResidency(page);
        if (written) {
            if (frame - state.armed_frame >= QUIET_FRAMES) {
                state.strikes = 0;
            }
            state.strikes = (std::min)(state.strikes + 1, MAX_STRIKES);
            state.retry_frame = frame + (u64{1} << state.strikes) - 1;
        }
    }

    [[nodiscard]] u32 ArmedPages() const noexcept {
        return armed_pages;
    }

    std::vector<u64> read_descriptors;
    std::vector<u64> resident; ///< Bits of IsResident
    std::vector<T> descriptors;
    std::vector<Id> bound_ids;
    std::vector<DescriptorPoolPage> pages;
    std::vector<Id> mirrored_ids;   ///< What the backend's heap holds per entry
    std::vector<u64> mirror_queued; ///< Bits of the entries in mirror_queue
    std::vector<u32> mirror_queue;  ///< Entries to send to the heap
    bool mirror_reset{};
    GPUVAddr current_gpu_addr{};
    u32 current_limit{};
    u32 armed_pages{};
    bool invalidated{}; ///< read_descriptors is to be cleared before the next read

private:
    void ClearReadDescriptors() noexcept {
        // Reserved capacity is not addressable. A later limit increase clears its new range.
        const size_t words = (std::min)(read_descriptors.size(), size_t{current_limit} / 64 + 1);
        std::fill_n(read_descriptors.begin(), words, u64{0});
        std::fill_n(resident.begin(), (std::min)(words, resident.size()), u64{0});
        const size_t ids = (std::min)(bound_ids.size(), size_t{current_limit} + 1);
        std::fill_n(bound_ids.begin(), ids, Id{0});
    }

    void ClearPageResidency(u32 page) noexcept {
        const u64 base = current_gpu_addr & (PAGE_SIZE - 1);
        const u64 begin = u64{page} << PAGE_BITS;
        const u64 first = begin > base ? (begin - base) / sizeof(T) : 0;
        const u64 last = (std::min)((begin + PAGE_SIZE - 1 - base) / sizeof(T), u64{current_limit});
        for (u64 index = first; index <= last && index / 64 < resident.size(); ++index) {
            resident[index / 64] &= ~(u64{1} << (index % 64));
        }
    }
};

} // namespace VideoCommon
