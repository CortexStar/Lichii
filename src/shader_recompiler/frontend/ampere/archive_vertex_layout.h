// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Shader::Ampere {

enum class ArchiveVertexType { Float, SignedInt, UnsignedInt };

struct ArchiveVertexAttribute {
    std::string shader_name;
    std::string source_name;
    ArchiveVertexType type;
    u16 format;
    u16 buffer_index;
    u16 offset;
    u32 stride;
};

struct ArchiveVertexLayout {
    std::string model_name;
    std::string shape_name;
    std::string material_name;
    std::string shader_archive;
    std::string shading_model;
    u32 shape_index;
    u32 material_index;
    u32 vertex_buffer_index;
    std::vector<ArchiveVertexAttribute> attributes;
    // Default assignments supply no evidence of a vertex-buffer binding.
    std::vector<std::string> default_attributes;
    // Shader assignment names whose source attribute is absent from this FVTX.
    // Their types remain unknown; consumers must check every consumed input.
    std::vector<std::string> missing_attributes;
};

struct ArchiveAttributeLocation {
    std::string name;
    u8 index;
    s32 location;
};

struct ArchiveShaderVertexMap {
    std::string shader_archive;
    std::string shading_model;
    // Offset in the original FSHA input of this model's embedded BNSH container.
    // Match ArchiveProgram::container to this offset; individual variations must
    // still be read/validated by ReadBnsh, not inferred from attribute metadata.
    u64 shader_binary_offset;
    std::vector<ArchiveAttributeLocation> attributes;
};

struct ResolvedArchiveVertexAttribute {
    u32 location;
    ArchiveVertexAttribute attribute;
};

// Input must be decompressed FRES 10.4.0 or FSHA 9.1.1, respectively.
// Malformed, ambiguous or unsupported metadata throws std::runtime_error.
// Results own their strings and never retain spans into the input.
[[nodiscard]] std::vector<ArchiveVertexLayout> ReadBfresVertexLayouts(std::span<const u8> data);
[[nodiscard]] std::vector<ArchiveShaderVertexMap> ReadBfshaVertexAttributeMaps(
    std::span<const u8> data);

// Resolves only explicit material assignments. An absent location is unknown, not
// an assertion that the game disables that attribute. Runtime overrides remain possible.
[[nodiscard]] std::vector<ResolvedArchiveVertexAttribute> ResolveArchiveVertexLayout(
    const ArchiveVertexLayout& layout, std::span<const ArchiveShaderVertexMap> shaders);

} // namespace Shader::Ampere
