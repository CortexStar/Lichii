// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright (c) 2026 Valiantsin Dzerakh
// SPDX-License-Identifier: MIT
// (GrowHeapDef follows NX2navi's res_heapdef_build, runtime/resolution.c,
// github.com/tangerineturtle1/NX2navi)

#include <cstring>
#include <string>

#include "common/settings.h"
#include "common/zstd_compression.h"
#include "core/oot_ultrawide.h"

namespace Core::OotUltrawide {
namespace {

// BYML, version 2..7, little endian ("YB"): header {magic, u16 version, u32 key table, u32 string
// table, u32 root}; a hash node is {u8 0xC1, u24 count} and count entries {u24 key index, u8 type,
// u32 value}; an int (type 0xD1) is stored in the entry, so a size changes in place.
constexpr u8 HashNode = 0xC1;
constexpr u8 IntNode = 0xD1;
constexpr u8 StringTable = 0xC2;

struct Byml {
    std::vector<u8>& data;
    u32 keys{};

    std::optional<u32> U32(size_t offset) const {
        if (offset + 4 > data.size()) {
            return std::nullopt;
        }
        u32 value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    std::optional<std::string_view> Key(u32 index) const {
        const auto head = U32(keys);
        if (!head || (*head & 0xFF) != StringTable || index >= (*head >> 8)) {
            return std::nullopt;
        }
        const auto at = U32(keys + 4 + 4 * size_t{index});
        if (!at || keys + size_t{*at} >= data.size()) {
            return std::nullopt;
        }
        const char* begin = reinterpret_cast<const char*>(data.data()) + keys + *at;
        const size_t limit = data.size() - keys - *at;
        const void* end = std::memchr(begin, 0, limit);
        if (end == nullptr) {
            return std::nullopt;
        }
        return std::string_view{begin, static_cast<size_t>(static_cast<const char*>(end) - begin)};
    }

    /// The offset of the value of hash node `node`'s entry `name` of type `type`, or 0.
    size_t Field(u32 node, std::string_view name, u8 type) const {
        const auto head = U32(node);
        if (!head || (*head & 0xFF) != HashNode) {
            return 0;
        }
        for (u32 i = 0; i < (*head >> 8); ++i) {
            const auto entry = U32(node + 4 + 8 * size_t{i});
            if (!entry) {
                return 0;
            }
            if (Key(*entry & 0xFFFFFF) == name) {
                return (*entry >> 24) == type ? node + 8 + 8 * size_t{i} : 0;
            }
        }
        return 0;
    }

    size_t HeapSize(u32 heaps, std::string_view heap) const {
        const size_t at = Field(heaps, heap, HashNode);
        const auto node = at != 0 ? U32(at) : std::nullopt;
        return node ? Field(*node, "Size", IntNode) : 0;
    }
};

} // Anonymous namespace

bool UltrawideRendering() {
    return Settings::values.aspect_ratio.GetValue() == Settings::AspectRatio::R21_9 &&
           Settings::IsDockedMode();
}

u64 ApplicationMemoryExtra() {
    return Settings::CurrentProgramIsSwitch2() && UltrawideRendering() ? MemoryExtraBytes : 0;
}

std::optional<std::vector<u8>> GrowHeapDef(std::span<const u8> compressed, u32 extra_kib) {
    auto data = Common::Compression::TryDecompressDataZSTD(compressed, 64ULL << 20);
    if (!data || data->size() < 16 || std::memcmp(data->data(), "YB", 2) != 0) {
        return std::nullopt;
    }
    u16 version;
    std::memcpy(&version, data->data() + 2, sizeof(version));
    Byml byml{*data};
    const auto keys = byml.U32(4);
    const auto root = byml.U32(12);
    if (version < 2 || version > 7 || !keys || !root) {
        return std::nullopt;
    }
    byml.keys = *keys;
    const size_t heaps_at = byml.Field(*root, "Heaps", HashNode);
    const auto heaps = heaps_at != 0 ? byml.U32(heaps_at) : std::nullopt;
    if (!heaps) {
        return std::nullopt;
    }
    const size_t texture_at = byml.HeapSize(*heaps, "DynamicTextureAllocatorMem");
    const size_t graphics_at = byml.HeapSize(*heaps, "Graphics");
    if (texture_at == 0 || graphics_at == 0) {
        return std::nullopt;
    }
    for (const size_t at : {texture_at, graphics_at}) {
        s32 size;
        std::memcpy(&size, data->data() + at, sizeof(size));
        if (size <= 0) {
            return std::nullopt;
        }
        size += static_cast<s32>(extra_kib);
        std::memcpy(data->data() + at, &size, sizeof(size));
    }
    auto out = Common::Compression::CompressDataZSTDDefault(data->data(), data->size());
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

} // namespace Core::OotUltrawide
