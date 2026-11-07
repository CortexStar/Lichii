// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "common/polyfill_thread.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "shader_recompiler/frontend/ampere/instruction.h"
#include "shader_recompiler/stage.h"

namespace Shader::Ampere {
struct LoweredProgram;
struct ArchiveProgram;
} // namespace Shader::Ampere

namespace VideoCommon {

// NUP-owned, device-independent compiler work. Pipeline specialization still uses
// the live game's resource descriptors and render state.
class NupShaderCache {
public:
    NupShaderCache();
    ~NupShaderCache();

    bool Prepare(const FileSys::VirtualFile& file, const std::filesystem::path& path,
                 std::stop_token stop, const std::function<void(size_t, size_t)>& progress);
    std::unique_ptr<Shader::Ampere::LoweredProgram> Find(
        std::span<const Shader::Ampere::Instruction> native, Shader::Stage stage,
        bool geometry) const;
    // The spans and lowered program are valid only for the duration of the callback.
    void ForEachComputeVariant(
        std::stop_token stop,
        const std::function<void(std::span<const u8>, std::span<const u8>,
                                 const Shader::Ampere::LoweredProgram*)>& callback) const;
    bool ForEachGraphicsGroup(
        std::stop_token stop,
        const std::function<void(std::span<const Shader::Ampere::ArchiveProgram>)>& callback) const;
    // 0 is unknown; 1/2/3 identify float, signed and unsigned shader inputs.
    using VertexInputTypes = std::array<u8, 32>;
    bool PrepareVertexLayouts(std::stop_token stop,
                              const std::function<void(size_t, size_t)>& progress);
    bool ForEachGraphicsGroupWithVertexLayouts(
        std::stop_token stop,
        const std::function<void(std::span<const Shader::Ampere::ArchiveProgram>,
                                 std::span<const VertexInputTypes>)>& callback) const;
    std::optional<std::array<u8, 32>> Identity() const;
    bool HasProgram(std::span<const Shader::Ampere::Instruction> native, Shader::Stage stage) const;
    std::optional<std::vector<u8>> ReadDriverCache(const std::array<u8, 32>& profile) const;
    bool WriteDriverCache(const std::array<u8, 32>& profile, std::span<const u8> data,
                          std::stop_token stop = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCommon
