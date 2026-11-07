// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/shader_archive.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace Shader {

class MissingOfflineContext : public RuntimeError {
public:
    explicit MissingOfflineContext(const std::string& message) : RuntimeError("{}", message) {}
};

class OfflineReader {
public:
    explicit OfflineReader(std::span<const u8> bytes_) : bytes{bytes_} {}

    template <typename T>
    T Read() {
        T value;
        Copy(&value, sizeof(value));
        return value;
    }

    void Copy(void* destination, size_t size) {
        if (size > bytes.size())
            throw std::runtime_error("Truncated environment record");
        std::memcpy(destination, bytes.data(), size);
        bytes = bytes.subspan(size);
    }

    size_t Remaining() const {
        return bytes.size();
    }

private:
    std::span<const u8> bytes;
};

template <typename K, typename V>
V OfflineLookup(const std::map<K, V>& values, K key, std::string_view name) {
    const auto it = values.find(key);
    if (it == values.end()) {
        throw MissingOfflineContext(fmt::format("Uncaptured {} at {:#x}", name, key));
    }
    return it->second;
}

inline u64 OfflineCbufKey(u32 bank, u32 offset) {
    return (u64{bank} << 32) | offset;
}

inline u64 OfflineIndirectCbufKey(u32 bank, u32 offset, u32 indirect) {
    return (u64{1} << 63) | (u64{indirect & 0x3fffff} << 40) | OfflineCbufKey(bank, offset);
}

class OfflineEnvironment final : public Shader::Environment {
public:
    OfflineEnvironment() = default;

    OfflineEnvironment(std::span<const u8> source, std::span<const u8> control,
                       const Ampere::LoweredProgram& lowered) {
        const auto sections = Ampere::ReadArchiveCodeSections(source, control);
        const auto groups = Ampere::ReadDeclaredComputeWorkgroup(source, control);
        const auto bindings = Ampere::ReadDeclaredComputeBindings(control);
        if (!sections || !groups || !bindings || !lowered ||
            lowered.source_size > sections->instruction_bytes) {
            throw MissingOfflineContext("Incomplete native compute metadata");
        }
        ValidateComputeMemory(lowered);
        SetStage(Stage::Compute);
        SetConstantWords(1, sections->constants);
        SetWorkgroup(*groups);
        SetDeclaredComputeBindings(*bindings);
        SetCode(lowered);
    }

    static void ValidateComputeMemory(const Ampere::LoweredProgram& lowered) {
        for (const u64 instruction : lowered.instructions) {
            using Opcode = Maxwell::Opcode;
            switch (Maxwell::Decode(instruction)) {
            case Opcode::LDL:
            case Opcode::STL:
                throw MissingOfflineContext("Archive control does not establish the local memory size");
            case Opcode::LDS:
            case Opcode::STS:
            case Opcode::ATOMS:
            case Opcode::ATOMS_cas:
                throw MissingOfflineContext("Archive control does not establish the shared memory size");
            default:
                break;
            }
        }
    }

    void Load(std::span<const u8> bytes, u32 version) {
        OfflineReader reader{bytes};
        const u64 size = reader.Read<u64>();
        const u64 type_count = reader.Read<u64>();
        const u64 format_count = reader.Read<u64>();
        const u64 cbuf_count = reader.Read<u64>();
        const u64 pointer_count = version >= 47 ? reader.Read<u64>() : 0;
        const u64 replacement_count = reader.Read<u64>();
        if (size % 8 || !size || size > reader.Remaining()) {
            throw std::runtime_error("Invalid environment code size");
        }
        for (const u64 count :
             {type_count, format_count, cbuf_count, pointer_count, replacement_count}) {
            if (count > bytes.size() / 8)
                throw std::runtime_error("Invalid environment entry count");
        }
        local_memory = reader.Read<u32>();
        texture_bound = reader.Read<u32>();
        start_address = reader.Read<u32>();
        low = reader.Read<u32>();
        high = reader.Read<u32>();
        viewport = reader.Read<u32>();
        const u32 stage_value = reader.Read<u32>();
        if (stage_value > static_cast<u32>(Stage::Mesh))
            throw std::runtime_error("Invalid cached stage");
        stage = static_cast<Stage>(stage_value);
        code.resize(size / 8);
        reader.Copy(code.data(), size);
        if (low % 8 || high < low || u64{high} - low + 8 != size) {
            throw std::runtime_error("Invalid cached instruction bounds");
        }
        for (u64 i = 0; i < type_count; ++i) {
            const u32 key = reader.Read<u32>();
            const u32 type = reader.Read<u32>();
            if (type >= Shader::NUM_TEXTURE_TYPES)
                throw std::runtime_error("Invalid texture type");
            texture_types.emplace(key, static_cast<Shader::TextureType>(type));
        }
        for (u64 i = 0; i < format_count; ++i) {
            const u32 key = reader.Read<u32>();
            const u32 value = reader.Read<u32>();
            if (value > std::numeric_limits<u8>::max()) {
                throw std::runtime_error("Invalid texture pixel format");
            }
            texture_formats.emplace(key, static_cast<Shader::TexturePixelFormat>(value));
        }
        for (u64 i = 0; i < cbuf_count; ++i) {
            const u64 key = reader.Read<u64>();
            cbuf_values.emplace(key, reader.Read<u32>());
        }
        for (u64 i = 0; i < pointer_count; ++i) {
            const u64 key = reader.Read<u64>();
            const u8 value = reader.Read<u8>();
            if (value > 1)
                throw std::runtime_error("Invalid storage pointer classification");
            pointers.emplace(key, value);
        }
        for (u64 i = 0; i < replacement_count; ++i) {
            const u64 key = reader.Read<u64>();
            const u32 value = reader.Read<u32>();
            if (value > 2)
                throw std::runtime_error("Invalid constant replacement");
            replacements.emplace(key, static_cast<Shader::ReplaceConstant>(value));
        }
        if (stage == Stage::Compute) {
            workgroup = reader.Read<std::array<u32, 3>>();
            shared_memory = reader.Read<u32>();
            if (!workgroup[0] || !workgroup[1] || !workgroup[2]) {
                throw std::runtime_error("Invalid cached workgroup dimensions");
            }
        } else {
            sph = reader.Read<Shader::ProgramHeader>();
            if (stage == Stage::Geometry)
                gp_passthrough_mask = reader.Read<std::array<u32, 8>>();
            if (stage == Stage::Task || stage == Stage::Mesh) {
                if (version < 53)
                    throw std::runtime_error("Task/mesh environments require cache version 53");
                mesh_registers = reader.Read<std::array<u32, 3>>();
                mesh_primitive_header = reader.Read<Shader::ProgramHeader>();
            }
        }
        if (reader.Remaining())
            throw std::runtime_error("Trailing bytes after environment record");
        is_proprietary_driver = texture_bound == 2 || IsNativeProgram();
        captured = true;
    }

    void SetStage(Stage value) {
        stage = value;
    }

    void SetWorkgroup(std::array<u32, 3> value) {
        workgroup = value;
    }

    void SetTextureContract(TextureSpecialization value) { texture_contract = value; }

    std::optional<TextureSpecialization> DeclaredTextureSpecialization(
        const ConstBufferAddr& address) override {
        const auto table_row = [](u32 index, u32 offset) {
            return index == 2 && offset >= 0x20 && offset < 0x968 && offset % 8 == 0;
        };
        if (!texture_contract || address.handle_program != ~0U || address.count != 1 ||
            !address.dynamic_offset.IsEmpty() ||
            !table_row(address.index, address.offset) ||
            (address.has_secondary && !table_row(address.secondary_index, address.secondary_offset))) {
            return std::nullopt;
        }
        const HandleKey key{address.index, address.offset, address.shift_left,
                            address.secondary_index, address.secondary_offset,
                            address.secondary_shift_left, address.count, address.has_secondary};
        if (std::ranges::find(required_textures, key) == required_textures.end()) {
            required_textures.push_back(key);
        }
        return texture_contract;
    }

    void SetMeshState(std::array<u32, 3> registers, std::span<const u8> primitive_header) {
        if (!primitive_header.empty() && primitive_header.size() < sizeof(mesh_primitive_header)) {
            throw MissingOfflineContext("Incomplete mesh primitive header");
        }
        mesh_registers = registers;
        mesh_primitive_header = {};
        if (!primitive_header.empty()) {
            std::memcpy(&mesh_primitive_header, primitive_header.data(), sizeof(mesh_primitive_header));
        }
    }

    void SetConstantWords(u32 bank, std::span<const u8> bytes) {
        for (size_t offset = 0; offset < bytes.size(); offset += sizeof(u32)) {
            u32 value;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            cbuf_values.emplace(OfflineCbufKey(bank, static_cast<u32>(offset)), value);
        }
    }

    void SetDeclaredComputeBindings(const Shader::Ampere::ArchiveComputeBindings& bindings) {
        // Offline declaration evidence, not a claim that a live address is mapped.
        // Production reuse also needs a binder that handles null/inactive resources.
        static_resource_layout = true;
        DeclareStoragePointers(bindings.first_pointer, bindings.pointer_count);
    }

    void DeclareStoragePointers(u32 first, u32 count) {
        for (u32 index = 0; index < count; ++index) {
            const u32 offset = first + index * 16;
            pointers.emplace(OfflineCbufKey(0, offset), 1);
            pointers.emplace(OfflineCbufKey(0, offset + 8), 0);
        }
    }

    void SetHeader(std::span<const u8> bytes) {
        std::memcpy(&sph, bytes.data(), sizeof(sph));
        const u64 size = sph.LocalMemorySize() + sph.common3.shader_local_memory_crs_size;
        if (size > std::numeric_limits<u32>::max())
            throw std::runtime_error("Local memory overflow");
        local_memory = static_cast<u32>(size);
    }

    void SetCode(const Shader::Ampere::LoweredProgram& lowered) {
        code.resize(sizeof(sph) / sizeof(u64));
        std::memcpy(code.data(), &sph, sizeof(sph));
        for (const u64 instruction : lowered.instructions) {
            if (code.size() % 4 == 0)
                code.push_back(0);
            code.push_back(instruction);
        }
        code.push_back(0);
        Shader::Ampere::AppendNativeAddressMap(code, lowered.source_instruction_offsets);
        low = 0;
        high = static_cast<u32>(code.size() * 8 - 8);
        start_address = stage == Stage::Compute ? sizeof(sph) : 0;
        if (lowered.required_texture_buffer != ~0U)
            texture_bound = lowered.required_texture_buffer;
        is_proprietary_driver = true;
    }

    u32 EntryPoint() const {
        return start_address + (stage == Stage::Compute ? 0 : sizeof(sph));
    }

    u64 ReadInstruction(u32 address) override {
        if (address < low || address > high || (address - low) % 8) {
            throw std::runtime_error(
                fmt::format("Instruction address outside capture: {:#x}", address));
        }
        return code.at((address - low) / 8);
    }
    u32 TranslateProgramAddress(u32 address) const override {
        if (!IsNativeProgram())
            return address;
        const auto result = Shader::Ampere::NativeProgramAddress(code, address);
        if (!result)
            throw std::runtime_error("Invalid native indirect branch target");
        return *result;
    }
    bool IsNativeProgram() const override {
        return Shader::Ampere::HasNativeAddressMap(code);
    }
    bool HasNativeInstructions() const override {
        return IsNativeProgram();
    }
    bool UsesStaticResourceLayout() const override {
        return static_resource_layout;
    }
    bool IsCbufStorageBufferPointer(u32 bank, u32 offset) override {
        return OfflineLookup(pointers, OfflineCbufKey(bank, offset), "storage pointer classification") != 0;
    }
    bool IsCbufIndirectStorageBufferPointer(u32 bank, u32 offset, u32 indirect) override {
        return OfflineLookup(pointers, OfflineIndirectCbufKey(bank, offset, indirect),
                      "storage pointer table classification") != 0;
    }
    u32 ReadCbufValue(u32 bank, u32 offset) override {
        const u64 key = OfflineCbufKey(bank, offset);
        const u32 value = OfflineLookup(cbuf_values, key, "constant buffer value");
        required_constants.emplace(key, value);
        return value;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        return OfflineLookup(texture_types, handle, "texture type");
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        return OfflineLookup(texture_formats, handle, "texture pixel format");
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        using F = Shader::TexturePixelFormat;
        switch (ReadTexturePixelFormat(handle)) {
        case F::A8B8G8R8_SINT:
        case F::A8B8G8R8_UINT:
        case F::A2B10G10R10_UINT:
        case F::R8_SINT:
        case F::R8_UINT:
        case F::R16G16B16A16_SINT:
        case F::R16G16B16A16_UINT:
        case F::R32G32B32A32_UINT:
        case F::R32G32B32A32_SINT:
        case F::R32G32_SINT:
        case F::R16_UINT:
        case F::R16_SINT:
        case F::R16G16_UINT:
        case F::R16G16_SINT:
        case F::R8G8_SINT:
        case F::R8G8_UINT:
        case F::R32G32_UINT:
        case F::R32_UINT:
        case F::R32_SINT:
            return true;
        default:
            return false;
        }
    }
    u32 ReadViewportTransformState() override {
        return viewport;
    }
    u32 TextureBoundBuffer() const override {
        if (texture_bound == ~0U)
            throw MissingOfflineContext("Uncaptured texture-bound constant buffer");
        return texture_bound;
    }
    u32 LocalMemorySize() const override {
        return local_memory;
    }
    u32 SharedMemorySize() const override {
        return shared_memory;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return workgroup;
    }
    bool HasHLEMacroState() const override {
        return !replacements.empty();
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override {
        const auto it = replacements.find(OfflineCbufKey(bank, offset));
        return it == replacements.end() ? std::nullopt : std::optional{it->second};
    }
    void Dump(u64, u64) override {}

    const std::map<u64, u32>& RequiredConstants() const { return required_constants; }
    const std::vector<HandleKey>& RequiredTextures() const { return required_textures; }

    bool captured{};
    std::vector<u64> code;

private:
    std::map<u64, u32> required_constants;
    std::optional<TextureSpecialization> texture_contract;
    std::vector<HandleKey> required_textures;
    bool static_resource_layout{};
    u32 low{}, high{}, local_memory{}, shared_memory{}, viewport{1}, texture_bound{~0U};
    std::array<u32, 3> workgroup{1, 1, 1};
    std::map<u32, Shader::TextureType> texture_types;
    std::map<u32, Shader::TexturePixelFormat> texture_formats;
    std::map<u64, u32> cbuf_values;
    std::map<u64, u8> pointers;
    std::map<u64, Shader::ReplaceConstant> replacements;
};

} // namespace Shader
