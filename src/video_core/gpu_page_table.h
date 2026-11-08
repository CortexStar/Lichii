// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <memory>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

/// The GPU page table: guest GPU virtual addresses to host device addresses, read by shaders
/// (global memory and storage buffers through VK_KHR_buffer_device_address) instead of binding
/// buffers per draw.
///
/// Two levels over the 40-bit GPU address space with 4 KiB pages:
///  - L1: one u32 per 2 MiB of address space, the index of that range's L2 block (0: none).
///  - L2 blocks: 512 u64 each, the host device address of each 4 KiB page. Block 0 is the null
///    block, every entry the null page.
/// A shader translates `va` with two dependent loads:
///     block = L1[va >> 21]
///     page = L2[block * 512 + ((va >> 12) & 511)]
///     ptr = page + (va & 4095)
/// Pages that are not resident point at the null page (zeroes), never at freed memory.
///
/// This class holds the table's image in host memory and which bytes of it changed. The renderer
/// uploads those bytes into the device copy in command order, so a draw recorded before a change
/// never reads the entry the change wrote. Not thread-safe: the GPU thread owns it.
class GpuPageTable {
public:
    static constexpr u32 ADDRESS_BITS = 40;
    static constexpr u32 PAGE_BITS = 12;
    static constexpr u64 PAGE_SIZE = u64{1} << PAGE_BITS;
    static constexpr u32 BLOCK_BITS = 21; ///< address bits one L2 block covers
    static constexpr u32 ENTRIES_PER_BLOCK = 1U << (BLOCK_BITS - PAGE_BITS);
    static constexpr u64 L1_ENTRIES = u64{1} << (ADDRESS_BITS - BLOCK_BITS);
    static constexpr u64 L1_BYTES = L1_ENTRIES * sizeof(u32);
    static constexpr u64 BLOCK_BYTES = ENTRIES_PER_BLOCK * sizeof(u64);

    /// A byte range of the table that changed (offset from the table's start).
    struct Update {
        u64 offset;
        u64 size;
    };

    /// `max_blocks` L2 blocks (the null block included) are allocated up front; each one covers
    /// 2 MiB of mapped address space.
    explicit GpuPageTable(u32 max_blocks);
    ~GpuPageTable();

    GpuPageTable(const GpuPageTable&) = delete;
    GpuPageTable& operator=(const GpuPageTable&) = delete;

    /// The address every entry that maps nothing holds. Rewrites the entries holding the old one.
    void SetNullPage(u64 device_address);

    /// Maps [gpu_addr, gpu_addr + size) to host device memory starting at `device_address`.
    /// Both ends are widened to whole pages: the page holding `gpu_addr` maps to
    /// `device_address` minus `gpu_addr`'s offset within it, whatever that address's own
    /// alignment. Returns false (nothing changed past the blocks it could get) when the L2 blocks
    /// ran out.
    bool Map(u64 gpu_addr, u64 size, u64 device_address);

    /// Points [gpu_addr, gpu_addr + size) (widened to whole pages) at the null page.
    void Unmap(u64 gpu_addr, u64 size);

    /// Points every page whose entry lies in [device_begin, device_end) at the null page: a host
    /// buffer holding those addresses is going away. Returns the number of pages it unmapped.
    u64 UnmapDevice(u64 device_begin, u64 device_end);

    /// What a shader reads for `gpu_addr`: the host device address, or the null page plus the
    /// page offset.
    [[nodiscard]] u64 Translate(u64 gpu_addr) const;

    /// Whether `gpu_addr`'s page maps somewhere other than the null page.
    [[nodiscard]] bool IsMapped(u64 gpu_addr) const;

    /// Marks every byte a shader can reach as changed (the device copy was recreated).
    void MarkAllDirty();

    /// Moves the changed byte ranges into `out` (sorted, merged) and forgets them.
    void TakeUpdates(std::vector<Update>& out);

    [[nodiscard]] bool HasUpdates() const noexcept {
        return l1_dirty_begin < l1_dirty_end || !dirty_blocks.empty();
    }

    /// The table's image: L1, then the L2 blocks.
    [[nodiscard]] std::span<const u8> Image() const noexcept {
        return {image.get(), TotalBytes()};
    }

    [[nodiscard]] u64 TotalBytes() const noexcept {
        return L1_BYTES + u64{max_blocks} * BLOCK_BYTES;
    }

    [[nodiscard]] u32 UsedBlocks() const noexcept {
        return used_blocks;
    }

    [[nodiscard]] u64 NullPage() const noexcept {
        return null_page;
    }

    /// Offset of the L2 pool from the table's start (what shaders add to the table address).
    static constexpr u64 BlockPoolOffset() noexcept {
        return L1_BYTES;
    }

private:
    [[nodiscard]] u32* L1() const noexcept {
        return reinterpret_cast<u32*>(image.get());
    }
    [[nodiscard]] u64* Block(u32 block) const noexcept {
        return reinterpret_cast<u64*>(image.get() + L1_BYTES + u64{block} * BLOCK_BYTES);
    }

    /// Forgets the extents overlapping guest pages [begin, end).
    void EraseExtents(u64 begin, u64 end);

    /// The L2 block for `l1_index`, allocated when `allocate`; 0 when there is none.
    u32 BlockFor(u64 l1_index, bool allocate);
    void MarkBlock(u32 block);

    std::unique_ptr<u8[]> image;
    u32 max_blocks;
    u32 used_blocks = 1; ///< the null block
    std::vector<u32> free_blocks;
    std::vector<u16> block_mapped; ///< per block: entries not pointing at the null page
    std::vector<u64> block_l1;     ///< per block: its L1 index
    std::vector<u32> dirty_blocks;
    std::vector<bool> block_dirty;
    u64 l1_dirty_begin = 0;
    u64 l1_dirty_end = 0;
    u64 null_page = 0;

    /// Ranges Map wrote whole and nothing changed since, by first guest byte (page aligned): a Map
    /// inside one with the same device offset changes nothing and skips the page walk (a 1 GiB
    /// pool binding is 262,144 entries).
    struct Extent {
        u64 end;    ///< guest end, page aligned
        u64 device; ///< device address of the first byte
    };
    std::map<u64, Extent> extents;
};

} // namespace VideoCommon
