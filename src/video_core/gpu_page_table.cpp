// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_core/gpu_page_table.h"

namespace VideoCommon {

// Shaders translate with the same layout (Shader::Backend::SPIRV::EmitContext::PageTableAddress).
static_assert(GpuPageTable::ADDRESS_BITS == Shader::Backend::SPIRV::PAGE_TABLE_ADDRESS_BITS);
static_assert(GpuPageTable::PAGE_BITS == Shader::Backend::SPIRV::PAGE_TABLE_PAGE_BITS);
static_assert(GpuPageTable::BLOCK_BITS == Shader::Backend::SPIRV::PAGE_TABLE_BLOCK_BITS);
static_assert(GpuPageTable::L1_BYTES == Shader::Backend::SPIRV::PAGE_TABLE_L1_BYTES);

GpuPageTable::GpuPageTable(u32 max_blocks_)
    : image{new u8[L1_BYTES + u64{(std::max)(max_blocks_, 1U)} * BLOCK_BYTES]{}},
      max_blocks{(std::max)(max_blocks_, 1U)}, block_mapped(max_blocks, 0), block_l1(max_blocks, 0),
      block_dirty(max_blocks, false) {
    // Everything starts unmapped: L1 all 0 (the null block) and the null block all null_page (0).
    l1_dirty_begin = 0;
    l1_dirty_end = L1_ENTRIES;
    MarkBlock(0);
}

GpuPageTable::~GpuPageTable() = default;

void GpuPageTable::MarkBlock(u32 block) {
    if (!block_dirty[block]) {
        block_dirty[block] = true;
        dirty_blocks.push_back(block);
    }
}

void GpuPageTable::SetNullPage(u64 device_address) {
    const u64 old_null = null_page;
    // Exact: the zero buffer is suballocated, and an address rounded down would point before it.
    null_page = device_address;
    if (old_null == null_page) {
        return;
    }
    std::fill_n(Block(0), ENTRIES_PER_BLOCK, null_page);
    MarkBlock(0);
    for (u32 block = 1; block < used_blocks; ++block) {
        u64* const entries = Block(block);
        bool changed = false;
        for (u32 entry = 0; entry < ENTRIES_PER_BLOCK; ++entry) {
            if (entries[entry] == old_null) {
                entries[entry] = null_page;
                changed = true;
            }
        }
        if (changed) {
            MarkBlock(block);
        }
    }
}

u32 GpuPageTable::BlockFor(u64 l1_index, bool allocate) {
    u32& slot = L1()[l1_index];
    if (slot != 0 || !allocate) {
        return slot;
    }
    u32 block;
    if (!free_blocks.empty()) {
        block = free_blocks.back();
        free_blocks.pop_back();
    } else if (used_blocks < max_blocks) {
        block = used_blocks++;
    } else {
        return 0;
    }
    std::fill_n(Block(block), ENTRIES_PER_BLOCK, null_page);
    block_mapped[block] = 0;
    block_l1[block] = l1_index;
    MarkBlock(block);
    slot = block;
    l1_dirty_begin =
        l1_dirty_begin < l1_dirty_end ? (std::min)(l1_dirty_begin, l1_index) : l1_index;
    l1_dirty_end = (std::max)(l1_dirty_end, l1_index + 1);
    return block;
}

void GpuPageTable::CutExtents(u64 begin, u64 end) {
    auto it = extents.upper_bound(begin);
    if (it != extents.begin()) {
        --it;
    }
    while (it != extents.end() && it->first < end) {
        const u64 extent_begin = it->first;
        const Extent extent = it->second;
        if (extent.end <= begin) {
            ++it;
            continue;
        }
        it = extents.erase(it);
        // The parts outside [begin, end) keep their mapping.
        if (extent_begin < begin) {
            extents.emplace(extent_begin, Extent{.end = begin, .device = extent.device});
        }
        if (extent.end > end) {
            it = extents
                     .emplace(end, Extent{.end = extent.end,
                                          .device = extent.device + (end - extent_begin)})
                     .first;
            ++it;
        }
    }
}

bool GpuPageTable::Map(u64 gpu_addr, u64 size, u64 device_address) {
    if (size == 0) {
        return true;
    }
    const u64 first = gpu_addr >> PAGE_BITS;
    const u64 limit = u64{1} << (ADDRESS_BITS - PAGE_BITS);
    const u64 end = (std::min)((gpu_addr + size + PAGE_SIZE - 1) >> PAGE_BITS, limit);
    if (first >= end) {
        return true;
    }
    const u64 begin_address = first << PAGE_BITS;
    const u64 end_address = end << PAGE_BITS;
    // The host address of the first guest page's first byte. Host buffers are suballocated at
    // their own alignment (often 256 bytes or less), so it need not be page aligned: an entry
    // holds the exact address, and a shader adds the guest page offset to it.
    u64 page_address = device_address - (gpu_addr & (PAGE_SIZE - 1));
    if (auto it = extents.upper_bound(begin_address); it != extents.begin()) {
        --it;
        if (it->second.end >= end_address &&
            it->second.device + (begin_address - it->first) == page_address) {
            return true;
        }
    }
    CutExtents(begin_address, end_address);
    const u64 first_page_address = page_address;
    for (u64 page = first; page < end && page < limit; ++page, page_address += PAGE_SIZE) {
        const u64 l1_index = page >> (BLOCK_BITS - PAGE_BITS);
        const u32 block = BlockFor(l1_index, true);
        if (block == 0) {
            // Out of blocks: the pages written so far stay mapped, and recorded.
            if (page > first) {
                extents.emplace(begin_address,
                                Extent{.end = page << PAGE_BITS, .device = first_page_address});
            }
            return false;
        }
        u64& entry = Block(block)[page & (ENTRIES_PER_BLOCK - 1)];
        if (entry == page_address) {
            continue;
        }
        if (entry == null_page) {
            ++block_mapped[block];
        } else if (page_address == null_page) {
            --block_mapped[block];
        }
        entry = page_address;
        MarkBlock(block);
    }
    extents.emplace(begin_address, Extent{.end = end_address, .device = first_page_address});
    return true;
}

void GpuPageTable::Unmap(u64 gpu_addr, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 first = gpu_addr >> PAGE_BITS;
    const u64 limit = u64{1} << (ADDRESS_BITS - PAGE_BITS);
    const u64 end = (std::min)((gpu_addr + size + PAGE_SIZE - 1) >> PAGE_BITS, limit);
    CutExtents(first << PAGE_BITS, end << PAGE_BITS);
    for (u64 page = first; page < end;) {
        const u64 l1_index = page >> (BLOCK_BITS - PAGE_BITS);
        const u64 block_end = (l1_index + 1) << (BLOCK_BITS - PAGE_BITS);
        const u32 block = BlockFor(l1_index, false);
        if (block == 0) {
            page = block_end;
            continue;
        }
        u64* const entries = Block(block);
        for (; page < end && page < block_end; ++page) {
            u64& entry = entries[page & (ENTRIES_PER_BLOCK - 1)];
            if (entry != null_page) {
                entry = null_page;
                --block_mapped[block];
                MarkBlock(block);
            }
        }
        if (block_mapped[block] == 0) {
            // Nothing left in it: the L1 entry goes back to the null block. The block is only
            // reused through an upload that comes after this one in command order.
            L1()[l1_index] = 0;
            block_l1[block] = 0;
            free_blocks.push_back(block);
            l1_dirty_begin =
                l1_dirty_begin < l1_dirty_end ? (std::min)(l1_dirty_begin, l1_index) : l1_index;
            l1_dirty_end = (std::max)(l1_dirty_end, l1_index + 1);
        }
    }
}

u64 GpuPageTable::UnmapDevice(u64 device_begin, u64 device_end) {
    if (device_begin >= device_end) {
        return 0;
    }
    // The extents record every mapped page: only the ones of this host range are walked, not
    // the whole table (a host buffer goes away many times a second while a scene loads).
    boost::container::small_vector<std::pair<u64, u64>, 8> ranges;
    for (const auto& [extent_begin, extent] : extents) {
        const u64 pages = (extent.end - extent_begin) >> PAGE_BITS;
        // Page k holds host bytes [device + k * PAGE_SIZE, + PAGE_SIZE).
        const u64 first =
            device_begin <= extent.device ? 0 : (device_begin - extent.device) >> PAGE_BITS;
        const u64 last = device_end <= extent.device
                             ? 0
                             : (device_end - extent.device + PAGE_SIZE - 1) >> PAGE_BITS;
        const u64 end_page = (std::min)(last, pages);
        if (first < end_page) {
            ranges.emplace_back(extent_begin + (first << PAGE_BITS), (end_page - first)
                                                                         << PAGE_BITS);
        }
    }
    u64 unmapped = 0;
    for (const auto& [gpu_addr, size] : ranges) {
        Unmap(gpu_addr, size);
        unmapped += size >> PAGE_BITS;
    }
    return unmapped;
}

u64 GpuPageTable::Translate(u64 gpu_addr) const {
    const u64 page = (gpu_addr >> PAGE_BITS) & ((u64{1} << (ADDRESS_BITS - PAGE_BITS)) - 1);
    const u32 block = L1()[page >> (BLOCK_BITS - PAGE_BITS)];
    return Block(block)[page & (ENTRIES_PER_BLOCK - 1)] + (gpu_addr & (PAGE_SIZE - 1));
}

bool GpuPageTable::IsMapped(u64 gpu_addr) const {
    return Translate(gpu_addr) - (gpu_addr & (PAGE_SIZE - 1)) != null_page;
}

void GpuPageTable::MarkAllDirty() {
    l1_dirty_begin = 0;
    l1_dirty_end = L1_ENTRIES;
    for (u32 block = 0; block < used_blocks; ++block) {
        MarkBlock(block);
    }
}

void GpuPageTable::TakeUpdates(std::vector<Update>& out) {
    out.clear();
    if (l1_dirty_begin < l1_dirty_end) {
        out.push_back(Update{
            .offset = l1_dirty_begin * sizeof(u32),
            .size = (l1_dirty_end - l1_dirty_begin) * sizeof(u32),
        });
    }
    std::ranges::sort(dirty_blocks);
    for (const u32 block : dirty_blocks) {
        block_dirty[block] = false;
        const u64 offset = L1_BYTES + u64{block} * BLOCK_BYTES;
        if (!out.empty() && out.back().offset + out.back().size == offset) {
            out.back().size += BLOCK_BYTES;
        } else {
            out.push_back(Update{.offset = offset, .size = BLOCK_BYTES});
        }
    }
    dirty_blocks.clear();
    l1_dirty_begin = 0;
    l1_dirty_end = 0;
}

} // namespace VideoCommon
