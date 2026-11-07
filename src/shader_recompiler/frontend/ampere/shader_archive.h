// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <array>
#include <optional>
#include <span>

#include "common/common_types.h"
#include "shader_recompiler/stage.h"

namespace Shader::Ampere {

struct ArchiveProgram {
    Stage stage;
    std::span<const u8> code;
    std::span<const u8> control;
    // Only BNSH variations establish a verified group of linked stages.
    std::span<const u8> container;
    std::optional<u32> variation;
};

struct ArchiveCodeSections {
    u32 instruction_bytes;
    std::span<const u8> constants;
};

struct ArchiveComputeBindings {
    u32 first_pointer;
    u32 pointer_count;
};

struct ArchiveMeshInfo {
    u32 threads;
    u32 payload_bytes;
    u32 max_vertices;
    u32 max_primitives;
    bool has_task;
    std::span<const u8> primitive_header;
};

// Spans belong to the input archive and are valid only during the callback.
// Returns false when the caller cancels. Malformed or unsupported archives throw.
bool ReadBnsh(std::span<const u8> data,
              const std::function<bool(const ArchiveProgram&)>& consume);
bool ReadEmbeddedBnsh(std::span<const u8> data,
                      const std::function<bool(const ArchiveProgram&)>& consume);
bool ReadShaderArchive(std::span<const u8> data,
                       const std::function<bool(const ArchiveProgram&)>& consume);
std::optional<std::array<u32, 3>> ReadDeclaredComputeWorkgroup(
    std::span<const u8> code, std::span<const u8> control);
std::optional<ArchiveCodeSections> ReadArchiveCodeSections(
    std::span<const u8> code, std::span<const u8> control);
std::optional<ArchiveComputeBindings> ReadDeclaredComputeBindings(std::span<const u8> control);
std::optional<ArchiveMeshInfo> ReadDeclaredMeshInfo(std::span<const u8> code,
                                                 std::span<const u8> control);

} // namespace Shader::Ampere
