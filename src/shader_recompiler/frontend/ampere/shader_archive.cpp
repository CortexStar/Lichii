// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/ampere/shader_archive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace Shader::Ampere {
namespace {

std::span<const u8> Slice(std::span<const u8> data, u64 offset, u64 size) {
    if (offset > data.size() || size > data.size() - offset) {
        throw std::runtime_error("Shader archive extent is out of bounds");
    }
    return data.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
}

template <typename T>
T Read(std::span<const u8> data, u64 offset) {
    static_assert(std::endian::native == std::endian::little);
    T value;
    std::memcpy(&value, Slice(data, offset, sizeof(T)).data(), sizeof(T));
    return value;
}

constexpr std::array<u8, 8> BnshMagic{'B', 'N', 'S', 'H', 0, 0, 0, 0};

} // namespace

bool ReadBnsh(std::span<const u8> data,
              const std::function<bool(const ArchiveProgram&)>& consume) {
    Slice(data, 0, 0x98);
    const u32 version = Read<u32>(data, 8);
    if (!std::ranges::equal(data.first(8), BnshMagic) ||
        Read<u32>(data, 0x60) != 0x63737267 ||
        Read<u16>(data, 0xc) != 0xfeff || Read<u32>(data, 0x1c) != data.size() ||
        (version != 0x00020400 && version != 0x00020301 && version != 0x00020401) ||
        Read<u16>(data, 0x70) != 4 || Read<u16>(data, 0x72) != 200) {
        throw std::runtime_error("Unsupported NVN2 BNSH header, version or API");
    }
    const u32 count = Read<u32>(data, 0x7c);
    const u64 variations = Read<u64>(data, 0x80);
    if (count && !variations) {
        throw std::runtime_error("Missing BNSH variation table");
    }
    Slice(data, variations, static_cast<u64>(count) * 0x40);
    for (u32 variation = 0; variation < count; ++variation) {
        const u64 program = Read<u64>(data, variations + static_cast<u64>(variation) * 0x40 + 0x10);
        if (!program) {
            throw std::runtime_error("Missing BNSH program");
        }
        Slice(data, program, 0x38);
        const bool mesh = version == 0x00020401 && Read<u64>(data, program) == 2;
        if (mesh) {
            Slice(data, program, 0x50);
        }
        for (u32 index = 0; index < (mesh ? 8U : 6U); ++index) {
            const u32 slot = index < 6 ? 8 + index * 8 : 0x40 + (index - 6) * 8;
            const u64 record = Read<u64>(data, program + slot);
            if (!record) {
                continue;
            }
            Slice(data, record, 0x40);
            const u64 control_offset = Read<u64>(data, record + 8);
            const u64 code_offset = Read<u64>(data, record + 16);
            const u32 code_size = Read<u32>(data, record + 24);
            const u32 control_size = Read<u32>(data, record + 28);
            if (!code_offset || !code_size || (control_size && !control_offset)) {
                throw std::runtime_error("Invalid BNSH stage allocation");
            }
            const auto code = Slice(data, code_offset, code_size);
            const auto control = Slice(data, control_offset, control_size);
            if (index != 5) {
                if (code.size() <= 128 || (code.size() - 128) % 16) {
                    throw std::runtime_error("Invalid BNSH graphics code size");
                }
                const u32 header_stage = (Read<u32>(code, 0) >> 10) & 15;
                if (index < 6 && header_stage != index + 1) {
                    throw std::runtime_error("BNSH graphics stage disagrees with native header");
                }
                if (index >= 6 &&
                    (control.size() < 88 || Read<u64>(control, 0) != 0x000000c998761234ULL ||
                     (Read<u32>(control, 84) >> 16) != index ||
                     (header_stage != 1 && !(index == 7 && header_stage == 3)))) {
                    throw std::runtime_error("Invalid BNSH task or mesh metadata");
                }
            }
            const Stage stage = index < 6 ? static_cast<Stage>(index)
                                          : index == 6 ? Stage::Task : Stage::Mesh;
            if (!consume({stage, code, control, data, variation})) {
                return false;
            }
        }
    }
    return true;
}

bool ReadEmbeddedBnsh(std::span<const u8> data,
                      const std::function<bool(const ArchiveProgram&)>& consume) {
    size_t cursor = 0;
    bool found = false;
    while (cursor < data.size()) {
        const auto match = std::search(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                                       data.end(), BnshMagic.begin(), BnshMagic.end());
        if (match == data.end()) {
            break;
        }
        const size_t offset = static_cast<size_t>(match - data.begin());
        const u32 size = Read<u32>(data, offset + 0x1c);
        if (size < 0x98) {
            throw std::runtime_error("Invalid embedded BNSH size");
        }
        if (!ReadBnsh(Slice(data, offset, size), consume)) {
            return false;
        }
        found = true;
        cursor = offset + size;
    }
    if (!found) {
        throw std::runtime_error("Archive contains no supported BNSH");
    }
    return true;
}

namespace {
bool ReadSharcb(std::span<const u8> data,
                const std::function<bool(const ArchiveProgram&)>& consume) {
    Slice(data, 0, 32);
    if (Read<u32>(data, 4) != 11 || Read<u32>(data, 8) != data.size() ||
        Read<u32>(data, 12) != 5 || Read<u32>(data, 16) != 0) {
        throw std::runtime_error("Unsupported BAHS version, platform or size");
    }
    const u64 metadata = Read<u32>(data, 20);
    const u64 pool_size = Read<u32>(data, 24);
    const u64 pool = Read<u32>(data, 28);
    const u64 table = metadata + 0x150;
    if (metadata < 32 || table + 8 > pool || pool + pool_size != data.size()) {
        throw std::runtime_error("Invalid BAHS metadata or code pool");
    }
    const u64 table_size = Read<u32>(data, table);
    const u32 count = Read<u32>(data, table + 4);
    const u64 end = table + table_size;
    if (!count || table_size < 8 || end > pool || count > (table_size - 8) / 32) {
        throw std::runtime_error("Invalid BAHS record table");
    }
    u64 cursor = table + 8;
    for (u32 index = 0; index < count; ++index) {
        if (cursor + 32 > end) throw std::runtime_error("Truncated BAHS record");
        const u64 length = Read<u32>(data, cursor);
        const u32 slot = Read<u32>(data, cursor + 4);
        if (length < 32 || Read<u32>(data, cursor + 12) != length - 16 || cursor + length > end) {
            throw std::runtime_error("Invalid BAHS record size");
        }
        Stage stage;
        u32 control_stage;
        switch (slot) {
        case 0: stage = Stage::VertexB; control_stage = 0; break;
        case 1: stage = Stage::Fragment; control_stage = 1; break;
        case 3: stage = Stage::Compute; control_stage = 5; break;
        case 4: stage = Stage::Task; control_stage = 6; break;
        case 5: stage = Stage::Mesh; control_stage = 7; break;
        default: throw std::runtime_error("Unsupported BAHS stage");
        }
        const u64 descriptor = cursor + 16;
        const u64 code_relative = Read<u64>(data, descriptor);
        const u64 control_size = Read<u32>(data, descriptor + 8);
        const s64 control_offset = static_cast<s64>(descriptor) + Read<s32>(data, descriptor + 12);
        if (control_size < 104 || control_offset < static_cast<s64>(table + 8) ||
            static_cast<u64>(control_offset) > end || control_size > end - control_offset) {
            throw std::runtime_error("Invalid BAHS control extent");
        }
        const auto control = Slice(data, control_offset, control_size);
        if (Read<u64>(control, 0) != 0x000000c998761234ULL ||
            (Read<u32>(control, 84) >> 16) != control_stage) {
            throw std::runtime_error("BAHS control stage or version mismatch");
        }
        const u32 instructions = Read<u32>(control, 56);
        const u32 constants_begin = Read<u32>(control, 64);
        const u32 allocation = Read<u32>(control, 68);
        if (!instructions || instructions % 16 || constants_begin % 16 ||
            instructions > constants_begin || constants_begin > allocation ||
            code_relative > pool_size || allocation > pool_size - code_relative) {
            throw std::runtime_error("Invalid BAHS code allocation");
        }
        const auto code = Slice(data, pool + code_relative, allocation);
        if (stage != Stage::Compute) {
            if (code.size() <= 128 || (code.size() - 128) % 16) {
                throw std::runtime_error("Invalid BAHS graphics allocation");
            }
            const u32 header_stage = (Read<u32>(code, 0) >> 10) & 15;
            const bool valid = stage == Stage::Fragment ? header_stage == 5
                : stage == Stage::Mesh ? header_stage == 1 || header_stage == 3
                                      : header_stage == 1;
            if (!valid) throw std::runtime_error("BAHS graphics stage mismatch");
        }
        if (!consume({stage, code, control, data, std::nullopt})) return false;
        cursor += length;
    }
    if (cursor != end) throw std::runtime_error("Trailing BAHS record data");
    return true;
}

bool ReadArchive(std::span<const u8> data,
                 const std::function<bool(const ArchiveProgram&)>& consume, size_t depth,
                 bool& found) {
    if (depth > 16) throw std::runtime_error("Shader archive nesting limit exceeded");
    if (data.size() >= 4 && Read<u32>(data, 0) == 0x53484142) { // BAHS
        found = true;
        return ReadSharcb(data, consume);
    }
    if (data.size() >= 4 && Read<u32>(data, 0) == 0x43524153) { // SARC
        Slice(data, 0, 20);
        if (Read<u16>(data, 6) != 0xfeff || Read<u32>(data, 8) != data.size()) {
            throw std::runtime_error("Unsupported shader SARC header");
        }
        const u64 sfat = Read<u16>(data, 4);
        if (Read<u32>(data, sfat) != 0x54414653) {
            throw std::runtime_error("Invalid shader SARC table");
        }
        const u64 nodes = sfat + Read<u16>(data, sfat + 4);
        const u64 count = Read<u16>(data, sfat + 6);
        const u64 payload = Read<u32>(data, 12);
        if (nodes + count * 16 > payload || payload > data.size()) {
            throw std::runtime_error("Invalid shader SARC payload");
        }
        for (u64 index = 0; index < count; ++index) {
            const u64 begin = Read<u32>(data, nodes + index * 16 + 8);
            const u64 end = Read<u32>(data, nodes + index * 16 + 12);
            if (end < begin || end > data.size() - payload) {
                throw std::runtime_error("Invalid shader SARC member extent");
            }
            if (!ReadArchive(Slice(data, payload + begin, end - begin), consume, depth + 1, found)) {
                return false;
            }
        }
        return true;
    }
    if (std::search(data.begin(), data.end(), BnshMagic.begin(), BnshMagic.end()) != data.end()) {
        found = true;
        return ReadEmbeddedBnsh(data, consume);
    }
    return true;
}
} // namespace

bool ReadShaderArchive(std::span<const u8> data,
                       const std::function<bool(const ArchiveProgram&)>& consume) {
    bool found = false;
    const bool complete = ReadArchive(data, consume, 0, found);
    if (!found) throw std::runtime_error("Archive contains no supported shader container");
    return complete;
}

std::optional<std::array<u32, 3>> ReadDeclaredComputeWorkgroup(
    std::span<const u8> code, std::span<const u8> control) {
    if (control.size() < 100 || Read<u64>(control, 0) != 0x000000c998761234ULL ||
        (Read<u32>(control, 84) >> 16) != 5) {
        return std::nullopt;
    }
    const u32 code_size = Read<u32>(control, 56);
    if (!code_size || code_size > code.size() || code_size % 16) {
        throw std::runtime_error("Invalid compute control code extent");
    }
    const std::array dimensions{Read<u32>(control, 88), Read<u32>(control, 92),
                                Read<u32>(control, 96)};
    if (std::ranges::any_of(dimensions, [](u32 size) { return size == 0 || size > 1024; })) {
        throw std::runtime_error("Invalid declared compute workgroup");
    }
    return dimensions;
}

std::optional<ArchiveCodeSections> ReadArchiveCodeSections(
    std::span<const u8> code, std::span<const u8> control) {
    if (control.size() < 0x48 || Read<u64>(control, 0) != 0x000000c998761234ULL) {
        return std::nullopt;
    }
    const u32 instructions = Read<u32>(control, 0x38);
    const u32 constants_begin = Read<u32>(control, 0x40);
    const u32 constants_end = Read<u32>(control, 0x44);
    if (!instructions || instructions % 16 || instructions > code.size() ||
        constants_begin % 4 || constants_end % 4 || instructions > constants_begin ||
        constants_begin > constants_end ||
        (constants_begin != constants_end && constants_end > code.size())) {
        throw std::runtime_error("Invalid native code/constant section extents");
    }
    return ArchiveCodeSections{instructions, constants_begin == constants_end ? std::span<const u8>{}
        : code.subspan(constants_begin, constants_end - constants_begin)};
}

std::optional<ArchiveComputeBindings> ReadDeclaredComputeBindings(std::span<const u8> control) {
    if (control.size() < 0xbc || Read<u64>(control, 0) != 0x000000c998761234ULL ||
        (Read<u32>(control, 84) >> 16) != 5) {
        return std::nullopt;
    }
    const u32 push_bytes = (static_cast<u32>(control[0xa9]) * sizeof(u32) + 15U) & ~15U;
    const u32 first = 0x10 + push_bytes;
    const u32 count = static_cast<u32>(control[0xb1]) + control[0xb2];
    const u32 handles = static_cast<u32>(control[0xb3]) + control[0xb4] + control[0xb5] + control[0xb6];
    if (first + count * 16 + handles * 8 != Read<u16>(control, 0xba)) {
        throw std::runtime_error("Invalid native compute resource table extent");
    }
    return ArchiveComputeBindings{first, count};
}

std::optional<ArchiveMeshInfo> ReadDeclaredMeshInfo(std::span<const u8> code,
                                                 std::span<const u8> control) {
    if (control.size() < 0x69 || Read<u64>(control, 0) != 0x000000c998761234ULL) {
        return std::nullopt;
    }
    const u32 stage = Read<u32>(control, 84) >> 16;
    if (stage != 6 && stage != 7) return std::nullopt;
    ArchiveMeshInfo info{Read<u16>(control, 0x58), Read<u16>(control, 0x5a), 0, 0, false, {}};
    if (!info.threads || info.threads > 1024 || info.payload_bytes % 4) {
        throw std::runtime_error("Invalid archive mesh workgroup or payload");
    }
    if (stage == 7) {
        info.max_vertices = Read<u16>(control, 0x60);
        info.max_primitives = Read<u16>(control, 0x62);
        info.has_task = control[0x5e] != 0;
        if (!info.max_vertices || !info.max_primitives) {
            throw std::runtime_error("Invalid archive mesh output limits");
        }
        if (control[0x68] & 0x80) {
            const u32 offset = Read<u32>(control, 0x64);
            if (!offset) throw std::runtime_error("Missing archive per-primitive header");
            info.primitive_header = Slice(code, offset, 128);
            if (((Read<u32>(info.primitive_header, 0) >> 10) & 15) != 4) {
                throw std::runtime_error("Unsupported archive per-primitive header");
            }
        }
    }
    return info;
}

} // namespace Shader::Ampere
