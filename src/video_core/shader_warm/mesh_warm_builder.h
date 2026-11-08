// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <iosfwd>
#include <memory>
#include <stop_token>
#include "video_core/shader_warm/shader_archives.h"
#include "video_core/shader_warm/warm_records.h"

namespace VideoCommon::ShaderWarm {

struct MeshWarmStage {
    u32 stage{};
    u64 code_hash{};
    std::vector<u8> environment;
};

struct MeshWarmTemplate {
    GraphicsKey key{};
    std::vector<MeshWarmStage> stages;
};

// Captured state is a prediction input only. These records must never enter the draw cache.
std::vector<MeshWarmTemplate> ReadMeshWarmTemplates(
    const std::vector<std::filesystem::path>& caches);
std::optional<std::vector<u8>> MakeMeshWarmCandidate(
    std::span<const MeshWarmStage> variation, const MeshWarmTemplate& donor);

class MeshWarmRecipes {
public:
    explicit MeshWarmRecipes(const std::vector<std::filesystem::path>& caches,
                             size_t max_candidates = 512);
    ~MeshWarmRecipes();
    void AddVariation(std::span<const ShaderRecord> variation);
    // Writes complete records, following the caller's standard cache header.
    size_t Write(std::ostream& out, std::stop_token stop);
    size_t TemplateCount() const;
    size_t VariationCount() const;
    size_t Skipped() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCommon::ShaderWarm
