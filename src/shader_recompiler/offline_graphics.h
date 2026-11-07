// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/nvn.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/offline_environment.h"

namespace Shader {

// Types use the pipeline's two-bit encoding: 0 unknown, 1 float, 2 signed, 3 unsigned.
// Missing declarations are acceptable only for inputs the shader does not consume.
inline std::optional<u64> RequiredOfflineVertexTypes(const VaryingState& loads,
                                                     const std::array<u8, 32>& types) {
    u64 result = 0;
    for (size_t index = 0; index < types.size(); ++index) {
        if (!loads.Generic(index))
            continue;
        if (types[index] == 0 || types[index] > 3)
            return std::nullopt;
        result |= u64{types[index]} << (index * 2);
    }
    return result;
}

inline bool MatchesOfflineVertexTypes(const VaryingState& loads,
                                      const std::array<u8, 32>& declared,
                                      const std::array<AttributeType, 32>& emitted) {
    if (!RequiredOfflineVertexTypes(loads, declared)) return false;
    for (size_t index = 0; index < declared.size(); ++index) {
        if (loads.Generic(index) && emitted[index] != static_cast<AttributeType>(declared[index] - 1)) {
            return false;
        }
    }
    return true;
}

struct OfflineGraphicsLayout {
    std::array<const Ampere::ArchiveProgram*, 3> programs{};
    size_t count{};
    std::array<u32, 3> mesh_registers{};
    std::optional<Ampere::ArchiveMeshInfo> mesh;
};

inline OfflineGraphicsLayout GetOfflineGraphicsLayout(
    std::span<const Ampere::ArchiveProgram> group) {
    std::array<const Ampere::ArchiveProgram*, 9> stages{};
    for (const auto& program : group) {
        const auto index = static_cast<size_t>(program.stage);
        if (index >= stages.size() || stages[index])
            throw MissingOfflineContext("Duplicate graphics stage");
        stages[index] = &program;
    }
    OfflineGraphicsLayout layout;
    if (stages[0] && stages[4] && group.size() == 2) {
        layout.programs = {stages[0], stages[4], nullptr};
        layout.count = 2;
        return layout;
    }
    if (!stages[8] || !stages[4] || group.size() != (stages[7] ? 3U : 2U)) {
        throw MissingOfflineContext("Requires a vertex/fragment or task/mesh/fragment group");
    }
    layout.mesh = Ampere::ReadDeclaredMeshInfo(stages[8]->code, stages[8]->control);
    const auto task = stages[7] ? Ampere::ReadDeclaredMeshInfo(stages[7]->code, stages[7]->control)
                                : std::nullopt;
    if (!layout.mesh || layout.mesh->has_task != (stages[7] != nullptr) || (stages[7] && !task)) {
        throw MissingOfflineContext("Mesh/task declarations do not match the archive group");
    }
    const auto& mesh = *layout.mesh;
    if (!mesh.payload_bytes || (task && task->payload_bytes != mesh.payload_bytes) ||
        mesh.payload_bytes % 128 || mesh.payload_bytes / 128 > 0x1ff || mesh.threads > 63 ||
        (task && task->threads > 63) || mesh.max_vertices > 0x1ff || mesh.max_primitives > 0x3ff) {
        throw MissingOfflineContext(
            "Mesh metadata cannot be represented by the compiler's launch layout");
    }
    const u32 lines = mesh.payload_bytes / 128;
    // Triangles are a prewarm contract, never a claim about the live draw topology.
    layout.mesh_registers = {4U | (mesh.max_vertices << 4) | (mesh.max_primitives << 13),
                             mesh.threads << 20,
                             (task ? task->threads : 0) | (lines << 12) | (lines << 22)};
    if (task)
        layout.programs[layout.count++] = stages[7];
    layout.programs[layout.count++] = stages[8];
    layout.programs[layout.count++] = stages[4];
    return layout;
}

inline std::vector<Ampere::Instruction> CaptureOfflineGraphics(
    const Ampere::ArchiveProgram& source) {
    const auto sections = Ampere::ReadArchiveCodeSections(source.code, source.control);
    if (!sections || sections->instruction_bytes <= 128 ||
        (sections->instruction_bytes - 128) / 16 >
            Ampere::ProgramCaptureState::MaximumInstructions) {
        throw MissingOfflineContext("Graphics code and C1 extents are unavailable");
    }
    std::vector<Ampere::Instruction> native((sections->instruction_bytes - 128) / 16);
    std::memcpy(native.data(), source.code.data() + 128, native.size() * 16);
    Ampere::ProgramCaptureState capture;
    bool complete = false;
    for (u32 index = 0; index < native.size(); ++index) {
        const auto state = capture.Observe(native[index], index);
        if (state == Ampere::ProgramCaptureState::Result::InvalidTarget)
            break;
        if (state == Ampere::ProgramCaptureState::Result::Complete) {
            native.resize(index + 1);
            complete = true;
            break;
        }
    }
    if (!complete)
        throw MissingOfflineContext("Graphics native capture is incomplete");
    if (source.stage == Stage::Task || source.stage == Stage::Mesh) {
        for (const auto& instruction : native) {
            const u32 opcode = instruction.Bits(0, 12);
            if (opcode == 0x984 || opcode == 0x388 || opcode == 0x988 || opcode == 0x38c ||
                opcode == 0x98c || opcode == 0xf8c) {
                throw MissingOfflineContext("Mesh private shared-memory size is unavailable");
            }
        }
    }
    return native;
}

inline OfflineEnvironment MakeOfflineGraphicsEnvironment(const Ampere::ArchiveProgram& source,
                                                         const OfflineGraphicsLayout& layout,
                                                         const Ampere::LoweredProgram& lowered,
                                                         bool float_textures) {
    if (!lowered)
        throw MissingOfflineContext(std::string{lowered.error});
    const auto sections = Ampere::ReadArchiveCodeSections(source.code, source.control);
    if (!sections)
        throw MissingOfflineContext("Graphics C1 extent is unavailable");
    OfflineEnvironment env;
    env.SetStage(source.stage);
    env.SetHeader(source.code);
    env.SetConstantWords(1, sections->constants);
    env.SetCode(lowered);
    env.DeclareStoragePointers(NvnStorageBufferBase(source.stage), 16);
    if (float_textures)
        env.SetTextureContract({TextureType::Color2D, TexturePixelFormat::A8B8G8R8_UNORM, false});
    if (source.stage == Stage::Task || source.stage == Stage::Mesh) {
        env.SetMeshState(layout.mesh_registers, layout.mesh->primitive_header);
    }
    return env;
}

} // namespace Shader
