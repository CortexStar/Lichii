// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/ampere/archive_vertex_layout.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Shader::Ampere {
namespace {

[[noreturn]] void Invalid(std::string_view message) {
    throw std::runtime_error("Archive vertex layout: " + std::string{message});
}

class Reader {
public:
    explicit Reader(std::span<const u8> data_) : data{data_} {}

    std::span<const u8> Slice(u64 offset, u64 size) const {
        if (offset > data.size() || size > data.size() - offset)
            Invalid("out-of-range metadata");
        return data.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
    }

    u64 Integer(u64 offset, size_t size) const {
        const auto bytes = Slice(offset, size);
        u64 value = 0;
        for (size_t i = 0; i < size; ++i)
            value |= u64{bytes[i]} << (8 * i);
        return value;
    }
    u8 U8(u64 offset) const {
        return static_cast<u8>(Integer(offset, 1));
    }
    u16 U16(u64 offset) const {
        return static_cast<u16>(Integer(offset, 2));
    }
    u32 U32(u64 offset) const {
        return static_cast<u32>(Integer(offset, 4));
    }
    u64 U64(u64 offset) const {
        return Integer(offset, 8);
    }

    void Magic(u64 offset, std::string_view magic) const {
        const auto bytes = Slice(offset, magic.size());
        if (!std::equal(bytes.begin(), bytes.end(), magic.begin()))
            Invalid("unexpected signature");
    }

    void Array(u64 offset, u64 count, u64 stride) const {
        if ((count && !offset) || !stride || count > data.size() / stride)
            Invalid("invalid array extent");
        Slice(offset, count * stride);
    }

    std::string String(u64 offset) const {
        if (!offset)
            Invalid("missing or external name");
        const u16 length = U16(offset);
        const auto bytes = Slice(offset + 2, u64{length} + 1);
        if (!length || length > 4096 || bytes.back() ||
            std::find(bytes.begin(), bytes.end() - 1, 0) != bytes.end() - 1) {
            Invalid("invalid metadata name");
        }
        return {reinterpret_cast<const char*>(bytes.data()), length};
    }

    u32 DictionaryCount(u64 offset) const {
        if (!offset)
            return 0;
        if (U32(offset) != 0)
            Invalid("unsupported dictionary signature");
        const u32 count = U32(offset + 4);
        Array(offset + 8, u64{count} + 1, 16);
        return count;
    }

    std::vector<std::string> Names(u64 offset, u32 maximum) const {
        const u32 count = DictionaryCount(offset);
        if (count > maximum)
            Invalid("excessive dictionary entries");
        std::vector<std::string> names;
        std::set<std::string> unique;
        for (u32 i = 0; i < count; ++i) {
            auto name = String(U64(offset + 8 + (u64{i} + 1) * 16 + 8));
            if (!unique.insert(name).second)
                Invalid("duplicate dictionary name");
            names.push_back(std::move(name));
        }
        return names;
    }

    void Header(std::string_view magic, u32 version, u64 minimum) const {
        Slice(0, minimum);
        Magic(0, magic);
        if (U32(8) != version || U16(12) != 0xfeff || U32(28) != data.size()) {
            Invalid("unsupported version, byte order or file size");
        }
    }

private:
    std::span<const u8> data;
};

struct Format {
    ArchiveVertexType type;
    u32 bytes;
};

Format ReadFormat(u16 format) {
    // Stored bytes are numeric type, then channel format (nn::gfx).
    // The public noclip fres_nx/nngfx_enum.ts documents those fields.
    const u32 channel = format & 0xff;
    const u32 type = format >> 8;
    u32 bytes;
    switch (channel) {
    case 0x02:
        bytes = 1;
        break; // R8
    case 0x09:
    case 0x0a:
        bytes = 2;
        break; // RG8, R16
    case 0x0b:
    case 0x0e:
    case 0x12:
    case 0x14:
        bytes = 4;
        break;
    case 0x15:
    case 0x17:
        bytes = 8;
        break;
    case 0x18:
        bytes = 12;
        break;
    case 0x19:
        bytes = 16;
        break;
    default:
        Invalid("unsupported vertex channel format");
    }
    if (channel == 0x0e && type != 2)
        Invalid("unsupported packed vertex format");
    if ((type == 1 || type == 2) &&
        (channel == 0x14 || channel == 0x17 || channel == 0x18 || channel == 0x19)) {
        Invalid("unsupported normalized 32-bit vertex format");
    }
    if (type == 5 && channel != 0x0a && channel != 0x12 && channel != 0x14 && channel != 0x15 &&
        channel != 0x17 && channel != 0x18 && channel != 0x19) {
        Invalid("unsupported floating vertex format");
    }
    switch (type) {
    case 1:
    case 2:
    case 5:
        return {ArchiveVertexType::Float, bytes};
    case 3:
        return {ArchiveVertexType::UnsignedInt, bytes};
    case 4:
        return {ArchiveVertexType::SignedInt, bytes};
    default:
        Invalid("unsupported vertex numeric format");
    }
}

struct Material {
    std::string name;
    std::string archive;
    std::string shader;
    std::vector<std::pair<std::string, std::string>> assignments;
    std::vector<std::string> defaults;
};

Material ReadMaterial(const Reader& read, u64 offset) {
    // BfresLibrary Switch/Model/MaterialParserV10.cs: ShaderInfo + ShaderAssignV10.
    read.Slice(offset, 0xb0);
    read.Magic(offset, "FMAT");
    Material result;
    result.name = read.String(read.U64(offset + 8));
    const u64 info = read.U64(offset + 16);
    read.Array(info, 1, 80);
    const u64 assign = read.U64(info);
    read.Array(assign, 1, 88);
    result.archive = read.String(read.U64(assign));
    result.shader = read.String(read.U64(assign + 8));
    const auto names = read.Names(read.U64(assign + 48), 32);
    const u32 count = read.U8(info + 68);
    if (count > names.size())
        Invalid("attribute assignment count exceeds its dictionary");
    const u64 values = read.U64(info + 8);
    read.Array(values, count, 8);
    const u64 indices = read.U64(info + 16);
    if (indices)
        read.Array(indices, count + names.size(), 1);
    else if (count != names.size())
        Invalid("missing attribute assignment indices");
    for (size_t i = 0; i < names.size(); ++i) {
        const u32 index = indices ? read.U8(indices + count + i) : static_cast<u32>(i);
        if (index == 255) {
            result.defaults.push_back(names[i]);
            continue;
        }
        if (index >= count)
            Invalid("attribute assignment index out of range");
        result.assignments.emplace_back(names[i], read.String(read.U64(values + index * 8)));
    }
    return result;
}

std::vector<ArchiveVertexAttribute> ReadVertexBuffer(const Reader& read, u64 offset, u32 index) {
    const auto metadata = read.Slice(offset, 0x58);
    if (std::ranges::all_of(metadata, [](u8 value) { return value == 0; })) {
        Invalid("FVTX metadata absent");
    }
    read.Magic(offset, "FVTX");
    const u32 count = read.U8(offset + 76);
    const u32 buffers = read.U8(offset + 77);
    if (count > 32 || read.U16(offset + 78) != index || (count && !buffers)) {
        Invalid("unsupported vertex-buffer counts or index");
    }
    const u64 attributes = read.U64(offset + 8);
    const u64 strides = read.U64(offset + 56);
    read.Array(attributes, count, 16);
    read.Array(strides, buffers, 16);
    if (read.DictionaryCount(read.U64(offset + 16)) != count)
        Invalid("vertex attribute dictionary mismatch");
    std::vector<ArchiveVertexAttribute> result;
    std::set<std::string> names;
    for (u32 i = 0; i < count; ++i) {
        const u64 at = attributes + i * 16;
        auto name = read.String(read.U64(at));
        if (!names.insert(name).second)
            Invalid("duplicate vertex-buffer attribute");
        const u16 format = static_cast<u16>((u16{read.U8(at + 8)} << 8) | read.U8(at + 9));
        const auto decoded = ReadFormat(format);
        const u16 position = read.U16(at + 12);
        const u16 buffer = read.U16(at + 14);
        if (buffer >= buffers)
            Invalid("vertex attribute buffer index out of range");
        const u32 stride = read.U32(strides + buffer * 16);
        if (!stride || u64{position} + decoded.bytes > stride)
            Invalid("vertex attribute exceeds its stride");
        result.push_back({{}, std::move(name), decoded.type, format, buffer, position, stride});
    }
    return result;
}

} // namespace

std::vector<ArchiveVertexLayout> ReadBfresVertexLayouts(std::span<const u8> data) {
    Reader read{data};
    read.Header("FRES", 0x000a0400, 0xf0);
    const u64 models = read.U64(40);
    const u32 count = read.U16(220);
    read.Array(models, count, 0x78);
    if (read.DictionaryCount(read.U64(48)) != count)
        Invalid("model dictionary mismatch");
    std::vector<ArchiveVertexLayout> layouts;
    for (u32 model_index = 0; model_index < count; ++model_index) {
        const u64 model = models + model_index * 0x78;
        read.Magic(model, "FMDL");
        const auto model_name = read.String(read.U64(model + 8));
        const u64 vertices = read.U64(model + 32);
        const u64 shapes = read.U64(model + 40);
        const u64 materials = read.U64(model + 56);
        const u32 vertex_count = read.U16(model + 104);
        const u32 shape_count = read.U16(model + 106);
        const u32 material_count = read.U16(model + 108);
        if (shape_count > data.size() / 0x60 - layouts.size())
            Invalid("excessive shape aliases");
        read.Array(vertices, vertex_count, 0x58);
        read.Array(shapes, shape_count, 0x60);
        read.Array(materials, material_count, 0xb0);
        if (read.DictionaryCount(read.U64(model + 48)) != shape_count ||
            read.DictionaryCount(read.U64(model + 64)) != material_count)
            Invalid("shape/material dictionary mismatch");
        std::map<u32, Material> parsed_materials;
        std::map<u32, std::vector<ArchiveVertexAttribute>> parsed_vertices;
        for (u32 shape_index = 0; shape_index < shape_count; ++shape_index) {
            const u64 shape = shapes + shape_index * 0x60;
            read.Magic(shape, "FSHP");
            const u32 material_index = read.U16(shape + 82);
            const u32 vertex_index = read.U16(shape + 86);
            if (material_index >= material_count || vertex_index >= vertex_count ||
                read.U64(shape + 16) != vertices + vertex_index * 0x58)
                Invalid("ambiguous shape linkage");
            if (!parsed_materials.contains(material_index)) {
                parsed_materials.emplace(material_index,
                                         ReadMaterial(read, materials + material_index * 0xb0));
            }
            if (!parsed_vertices.contains(vertex_index)) {
                parsed_vertices.emplace(
                    vertex_index,
                    ReadVertexBuffer(read, vertices + vertex_index * 0x58, vertex_index));
            }
            const auto& material = parsed_materials.at(material_index);
            const auto& attributes = parsed_vertices.at(vertex_index);
            ArchiveVertexLayout layout{model_name,
                                       read.String(read.U64(shape + 8)),
                                       material.name,
                                       material.archive,
                                       material.shader,
                                       shape_index,
                                       material_index,
                                       vertex_index,
                                       {},
                                       material.defaults,
                                       {}};
            for (const auto& [shader_name, source_name] : material.assignments) {
                const auto found = std::ranges::find(attributes, source_name,
                                                     &ArchiveVertexAttribute::source_name);
                if (found == attributes.end()) {
                    layout.missing_attributes.push_back(shader_name);
                    continue;
                }
                auto attribute = *found;
                attribute.shader_name = shader_name;
                layout.attributes.push_back(std::move(attribute));
            }
            layouts.push_back(std::move(layout));
        }
    }
    return layouts;
}

std::vector<ArchiveShaderVertexMap> ReadBfshaVertexAttributeMaps(std::span<const u8> data) {
    Reader read{data};
    read.Header("FSHA", 0x00090101, 0x80);
    const auto archive = read.String(read.U64(0x38));
    const u64 models = read.U64(0x48);
    const u32 count = read.U16(0x78);
    read.Array(models, count, 0x100);
    if (read.DictionaryCount(read.U64(0x50)) != count)
        Invalid("shader model dictionary mismatch");
    std::vector<ArchiveShaderVertexMap> result;
    std::set<std::string> model_names;
    std::set<u64> binary_offsets;
    for (u32 i = 0; i < count; ++i) {
        const u64 model = models + i * 0x100;
        auto name = read.String(read.U64(model));
        if (!model_names.insert(name).second)
            Invalid("ambiguous shader model name");
        const u64 binary = read.U64(model + 168);
        read.Array(binary, 1, 0x98);
        read.Magic(binary, std::string_view{"BNSH\0\0\0\0", 8});
        const u32 binary_size = read.U32(binary + 28);
        if (binary_size < 0x98 || !binary_offsets.insert(binary).second) {
            Invalid("invalid or ambiguous shader binary extent");
        }
        read.Slice(binary, binary_size);
        const auto names = read.Names(read.U64(model + 48), 32);
        if (names.size() != read.U8(model + 236))
            Invalid("shader attribute count mismatch");
        const u64 attributes = read.U64(model + 40);
        read.Array(attributes, names.size(), 2);
        ArchiveShaderVertexMap shader{archive, std::move(name), binary, {}};
        std::set<u8> indices;
        std::set<s32> locations;
        for (size_t j = 0; j < names.size(); ++j) {
            const u8 index = read.U8(attributes + j * 2);
            const u8 raw = read.U8(attributes + j * 2 + 1);
            const s32 location = raw < 128 ? raw : s32{raw} - 256;
            if (!indices.insert(index).second || location < -1 || location >= 32 ||
                (location >= 0 && !locations.insert(location).second))
                Invalid("ambiguous shader attribute location");
            shader.attributes.push_back({names[j], index, location});
        }
        result.push_back(std::move(shader));
    }
    return result;
}

std::vector<ResolvedArchiveVertexAttribute> ResolveArchiveVertexLayout(
    const ArchiveVertexLayout& layout, std::span<const ArchiveShaderVertexMap> shaders) {
    const ArchiveShaderVertexMap* matched = nullptr;
    for (const auto& shader : shaders) {
        if (shader.shader_archive == layout.shader_archive &&
            shader.shading_model == layout.shading_model) {
            if (matched)
                Invalid("ambiguous matching shader model");
            matched = &shader;
        }
    }
    if (!matched)
        Invalid("no matching shader archive/model");
    std::set<std::string> names;
    std::set<s32> declared_locations;
    for (const auto& attribute : matched->attributes) {
        if (!names.insert(attribute.name).second || attribute.location < -1 ||
            attribute.location >= 32 ||
            (attribute.location >= 0 && !declared_locations.insert(attribute.location).second)) {
            Invalid("ambiguous shader attribute map");
        }
    }
    std::vector<ResolvedArchiveVertexAttribute> result;
    std::set<u32> locations;
    for (const auto& attribute : layout.attributes) {
        const auto found = std::ranges::find(matched->attributes, attribute.shader_name,
                                             &ArchiveAttributeLocation::name);
        if (found == matched->attributes.end())
            Invalid("material assignment absent from shader model");
        if (found->location < 0)
            continue;
        const u32 location = static_cast<u32>(found->location);
        if (location >= 32 || !locations.insert(location).second)
            Invalid("ambiguous resolved input location");
        result.push_back({location, attribute});
    }
    std::ranges::sort(result, {}, &ResolvedArchiveVertexAttribute::location);
    return result;
}

} // namespace Shader::Ampere
