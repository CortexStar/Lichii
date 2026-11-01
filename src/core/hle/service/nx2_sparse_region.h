// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/div_ceil.h"

namespace Service {

template <typename Unmap, typename Close>
bool Nx2TryReleaseBacking(Unmap&& unmap, Close&& close) {
    if (!unmap()) {
        return false;
    }
    close();
    return true;
}

struct Nx2SparseMapRequest {
    u64 gpu_address;
    u64 size;
    u8 no_write;
    u8 cacheable;
    u8 unknown_12;
    u8 kind;
    u8 fallback_kind;
    u8 unknown_15;
    u16 padding_16;
    u32 memory_handle;
    u32 padding_1c;
    u64 offset;

    bool Valid() const {
        return size != 0 && size <= (std::numeric_limits<u64>::max)() - gpu_address &&
               ((gpu_address | size) & 0xfff) == 0;
    }

    std::optional<u64> Backing(u64 address, u64 bound) const {
        if (!Valid() || address == 0 || bound > (std::numeric_limits<u64>::max)() - address ||
            offset > bound || size > bound - offset || ((address | offset) & 0xfff) != 0) {
            return std::nullopt;
        }
        return address + offset;
    }
};
static_assert(sizeof(Nx2SparseMapRequest) == 40);

inline std::optional<std::vector<Nx2SparseMapRequest>> Nx2ParseSparseMapRequests(
    std::span<const u8> data, u64 stride, u64 output_size) {
    if (stride != sizeof(Nx2SparseMapRequest) || data.size() % stride != 0 ||
        data.size() / stride > output_size) {
        return std::nullopt;
    }
    std::vector<Nx2SparseMapRequest> requests(data.size() / stride);
    if (!data.empty()) {
        std::memcpy(requests.data(), data.data(), data.size());
    }
    return requests;
}

struct Nx2SparseRegion {
    static constexpr u64 BlockSize = 4ULL << 20;

    enum class Block : u8 { Empty, Backed, Failed, Pending, Written };

    struct PendingBlock {
        u64 index;
        u64 generation;
        u64 size;
    };

    Nx2SparseRegion(u64 size_, u64 generation_)
        : size{size_}, blocks(Common::DivCeil(size, BlockSize)), generation{generation_} {}

    std::optional<PendingBlock> BeginBacking(u64 block) {
        if (tiles || block >= blocks.size() || blocks[block] != Block::Empty) {
            return std::nullopt;
        }
        blocks[block] = Block::Pending;
        return PendingBlock{block, generation, (std::min)(BlockSize, size - block * BlockSize)};
    }

    bool IsPending(const PendingBlock& pending) const {
        return !tiles && generation == pending.generation && pending.index < blocks.size() &&
               blocks[pending.index] == Block::Pending;
    }

    bool FinishBacking(const PendingBlock& pending, bool success) {
        if (!IsPending(pending)) {
            return false;
        }
        blocks[pending.index] = success ? Block::Backed : Block::Failed;
        return true;
    }

    void StartTiles() {
        tiles = true;
        std::fill(blocks.begin(), blocks.end(), Block::Empty);
    }

    bool MarkWritten(u64 offset, u64 length) {
        if (!tiles || length == 0 || offset >= size || length > size - offset) {
            return false;
        }
        for (u64 block = offset / BlockSize; block <= (offset + length - 1) / BlockSize; ++block) {
            blocks[block] = Block::Written;
        }
        return true;
    }

    u64 size;
    std::vector<Block> blocks;
    bool warned{};
    u64 generation;
    bool tiles{};
};

} // namespace Service
