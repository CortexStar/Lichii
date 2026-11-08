// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

#include "common/cityhash.h"
#include "video_core/shader_warm/mesh_warm_builder.h"

namespace VideoCommon::ShaderWarm {
namespace {
constexpr size_t HeaderSize = 76;
constexpr size_t MeshTailSize = 92;
constexpr size_t MaxRecordBytes = 16 * 1024 * 1024;
constexpr size_t MaxPreparedBytes = 128 * 1024 * 1024;

template <typename T>
T Get(std::span<const u8> bytes, size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template <typename T>
void Append(std::vector<u8>& bytes, const T& value) {
    const auto* data = reinterpret_cast<const u8*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(value));
}

bool MeshStages(std::span<const MeshWarmStage> stages) {
    return (stages.size() == 2 && stages[0].stage == 8 && stages[1].stage == 4) ||
           (stages.size() == 3 && stages[0].stage == 7 && stages[1].stage == 8 &&
            stages[2].stage == 4);
}

// Validates lengths before advancing over any table; also used on recorded, append-only caches.
std::optional<size_t> EnvSize(std::span<const u8> bytes) {
    if (bytes.size() < HeaderSize) {
        return std::nullopt;
    }
    const auto stage = Get<u32>(bytes, 72);
    if (stage > 8) {
        return std::nullopt;
    }
    size_t size = HeaderSize;
    constexpr std::array<u64, 6> strides{1, 8, 8, 12, 9, 12};
    for (size_t i = 0; i < strides.size(); ++i) {
        const u64 count = Get<u64>(bytes, i * 8);
        if (count > (std::min(bytes.size(), MaxRecordBytes) - size) / strides[i]) {
            return std::nullopt;
        }
        size += static_cast<size_t>(count * strides[i]);
    }
    const size_t tail = stage == 5 ? 16 : 80 + (stage == 3 ? 32 : 0) +
                                                     (stage == 7 || stage == 8 ? 92 : 0);
    if (tail > std::min(bytes.size(), MaxRecordBytes) - size) {
        return std::nullopt;
    }
    return size + tail;
}

std::span<const u8> MeshContext(const MeshWarmTemplate& donor) {
    const auto& mesh = donor.stages[donor.stages.size() - 2].environment;
    return std::span{mesh}.last(MeshTailSize);
}

u64 ContextHash(const MeshWarmTemplate& donor) {
    const auto context = MeshContext(donor);
    return Common::CityHash64(reinterpret_cast<const char*>(context.data()), context.size()) | 1;
}

bool ValidDonor(const MeshWarmTemplate& donor) {
    if (!MeshStages(donor.stages)) {
        return false;
    }
    for (const auto& stage : donor.stages) {
        const auto size = EnvSize(stage.environment);
        if (!size || *size != stage.environment.size() ||
            Get<u32>(stage.environment, 72) != stage.stage) {
            return false;
        }
    }
    if (donor.stages.size() == 3 &&
        std::memcmp(donor.stages[0].environment.data() +
                        donor.stages[0].environment.size() - MeshTailSize,
                    MeshContext(donor).data(), 12) != 0) {
        return false;
    }
    std::array<u64, 6> expected{};
    const u64 context = ContextHash(donor);
    for (size_t i = 0; i < donor.stages.size(); ++i) {
        const auto& stage = donor.stages[i];
        const size_t slot = stage.stage == 4 ? 5 : (i == 0 ? 1 : 3);
        expected[slot] = stage.code_hash ^ (stage.stage == 4 ? 0 : context);
    }
    return std::memcmp(expected.data(), donor.key.data(), sizeof(expected)) == 0;
}
} // namespace

std::vector<MeshWarmTemplate> ReadMeshWarmTemplates(
    const std::vector<std::filesystem::path>& caches) {
    std::vector<MeshWarmTemplate> result;
    std::set<GraphicsKey> seen;
    size_t retained_bytes = 0;
    for (const auto& path : caches) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in || in.tellg() < 12 || in.tellg() > 1024LL * 1024 * 1024) {
            continue;
        }
        std::vector<u8> bytes(static_cast<size_t>(in.tellg()));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
            std::memcmp(bytes.data(), "yuzucach", 8) != 0 ||
            !IsReadableRuntimeCacheVersion(Get<u32>(bytes, 8))) {
            continue;
        }
        size_t pos = 12;
        while (pos + 4 <= bytes.size()) {
            const u32 count = Get<u32>(bytes, pos);
            pos += 4;
            if (count == 0 || count > 6) {
                break;
            }
            std::vector<std::span<const u8>> envs;
            bool truncated = false;
            for (u32 i = 0; i < count; ++i) {
                const auto size = EnvSize(std::span{bytes}.subspan(pos));
                if (!size) {
                    truncated = true;
                    break;
                }
                envs.push_back(std::span{bytes}.subspan(pos, *size));
                pos += *size;
            }
            if (truncated) {
                break; // Ignore a partially appended tail; other source caches can still be read.
            }
            const size_t key_size = Get<u32>(envs.front(), 72) == 5 ? 24 : GraphicsKeySize;
            if (key_size > bytes.size() - pos) {
                break;
            }
            const auto key = std::span{bytes}.subspan(pos, key_size);
            pos += key_size;
            if ((count != 2 && count != 3) || key_size != GraphicsKeySize ||
                Get<u32>(envs[count - 2], 72) != 8 || Get<u32>(envs.back(), 72) != 4 ||
                (count == 3 && Get<u32>(envs[0], 72) != 7)) {
                continue;
            }
            MeshWarmTemplate donor;
            std::memcpy(donor.key.data(), key.data(), key.size());
            if (!seen.insert(donor.key).second) {
                continue;
            }
            size_t size = 0;
            for (const auto env : envs) {
                size += env.size();
                const u64 code_size = Get<u64>(env, 0);
                donor.stages.push_back({Get<u32>(env, 72),
                                       StageHash(env.subspan(HeaderSize, code_size)),
                                       {env.begin(), env.end()}});
            }
            if (size > MaxPreparedBytes - retained_bytes || result.size() >= 4096) {
                return result;
            }
            if (ValidDonor(donor)) {
                retained_bytes += size;
                result.push_back(std::move(donor));
            }
        }
    }
    return result;
}

std::optional<std::vector<u8>> MakeMeshWarmCandidate(
    std::span<const MeshWarmStage> variation, const MeshWarmTemplate& donor) {
    if (!MeshStages(variation) || variation.size() != donor.stages.size() || !ValidDonor(donor)) {
        return std::nullopt;
    }
    std::vector<u8> result;
    Append(result, static_cast<u32>(variation.size()));
    GraphicsKey key = donor.key;
    std::fill_n(key.begin(), 48, u8{0});
    const auto context = MeshContext(donor);
    const u64 context_hash = ContextHash(donor);
    for (size_t i = 0; i < variation.size(); ++i) {
        const auto& stage = variation[i];
        const auto& captured = donor.stages[i];
        std::vector<u8> env;
        if (stage.code_hash == captured.code_hash) {
            env = captured.environment; // Exact code: preserve the observed descriptor/cbuf reads.
        } else {
            env = stage.environment;
            // Fresh GraphicsEnv records have no mesh trailer until a candidate supplies it.
            if (env.size() < HeaderSize || Get<u32>(env, 72) != stage.stage) {
                return std::nullopt;
            }
            if (stage.stage == 7 || stage.stage == 8) {
                env.insert(env.end(), context.begin(), context.begin() + 12);
                if (stage.stage == 8) {
                    env.insert(env.end(), context.begin() + 12, context.end());
                } else {
                    env.resize(env.size() + 80, 0);
                }
            }
        }
        const auto size = EnvSize(env);
        if (!size || *size != env.size() || env.size() > MaxRecordBytes - result.size() ||
            StageHash(std::span{env}.subspan(HeaderSize, Get<u64>(env, 0))) != stage.code_hash) {
            return std::nullopt;
        }
        result.insert(result.end(), env.begin(), env.end());
        const size_t slot = stage.stage == 4 ? 5 : (i == 0 ? 1 : 3);
        const u64 hash = stage.code_hash ^ (stage.stage == 4 ? 0 : context_hash);
        if (hash == 0) {
            return std::nullopt;
        }
        std::memcpy(key.data() + slot * 8, &hash, sizeof(hash));
    }
    if (key.size() > MaxRecordBytes - result.size()) {
        return std::nullopt;
    }
    result.insert(result.end(), key.begin(), key.end());
    return result;
}

struct MeshWarmRecipes::Impl {
    std::vector<MeshWarmTemplate> donors;
    std::map<std::pair<u32, u128>, std::shared_ptr<MeshWarmStage>> programs;
    std::set<std::array<u64, 3>> seen_variations;
    std::vector<std::vector<std::shared_ptr<MeshWarmStage>>> variations;
    size_t bytes{};
    size_t skipped{};
    size_t max_candidates{};
};

MeshWarmRecipes::MeshWarmRecipes(const std::vector<std::filesystem::path>& caches, size_t limit)
    : impl{std::make_unique<Impl>()} {
    impl->donors = ReadMeshWarmTemplates(caches);
    impl->max_candidates = std::min<size_t>(limit, 4096);
}
MeshWarmRecipes::~MeshWarmRecipes() = default;

void MeshWarmRecipes::AddVariation(std::span<const ShaderRecord> variation) {
    if (impl->donors.empty() || impl->max_candidates == 0 || impl->variations.size() >= 4096) {
        return;
    }
    std::array<const ShaderRecord*, 3> slots{};
    for (const auto& record : variation) {
        const int slot = record.stage == ProgramStage::Task ? 0 :
                         record.stage == ProgramStage::Mesh ? 1 :
                         record.stage == ProgramStage::Fragment ? 2 : -1;
        if (slot >= 0) {
            if (slots[slot]) {
                ++impl->skipped;
                return;
            }
            slots[slot] = &record;
        }
    }
    if (!slots[1] || !slots[2]) {
        return;
    }
    std::vector<std::shared_ptr<MeshWarmStage>> prepared;
    for (size_t i = slots[0] ? 0 : 1; i < slots.size(); ++i) {
        const auto& record = *slots[i];
        const u32 stage = i == 0 ? 7 : i == 1 ? 8 : 4;
        const auto id = std::pair{stage, Common::CityHash128(
                                          reinterpret_cast<const char*>(record.code.data()),
                                          record.code.size())};
        auto [it, inserted] = impl->programs.try_emplace(id);
        if (inserted) {
            LoweredCode lowered;
            if (LowerProgram(record.code, true, lowered)) {
                ++impl->skipped;
                return;
            }
            auto env = GraphicsEnv(stage, lowered, record.code);
            if (env.size() > MaxPreparedBytes - impl->bytes) {
                ++impl->skipped;
                return;
            }
            impl->bytes += env.size();
            it->second = std::make_shared<MeshWarmStage>(
                MeshWarmStage{stage, StageHash(lowered.code), std::move(env)});
        }
        if (!it->second) {
            return;
        }
        prepared.push_back(it->second);
    }
    std::array<u64, 3> identity{};
    for (const auto& stage : prepared) {
        identity[stage->stage == 7 ? 0 : stage->stage == 8 ? 1 : 2] = stage->code_hash;
    }
    if (impl->seen_variations.insert(identity).second) {
        impl->variations.push_back(std::move(prepared));
    }
}

size_t MeshWarmRecipes::Write(std::ostream& out, std::stop_token stop) {
    struct Choice { size_t group; size_t donor; int score; };
    std::vector<Choice> choices;
    std::set<GraphicsKey> known;
    for (const auto& donor : impl->donors) {
        known.insert(donor.key);
    }
    for (size_t g = 0; g < impl->variations.size(); ++g) {
        if (stop.stop_requested()) {
            return 0;
        }
        const auto& group = impl->variations[g];
        std::vector<Choice> ranked;
        for (size_t d = 0; d < impl->donors.size(); ++d) {
            const auto& donor = impl->donors[d];
            if (donor.stages.size() != group.size()) {
                continue;
            }
            int score = 0;
            for (size_t i = 0; i < group.size(); ++i) {
                if (group[i]->code_hash == donor.stages[i].code_hash) {
                    score += group[i]->stage == 8 ? 16 : 8;
                }
            }
            // With no related code, these metadata guesses have no supporting evidence.
            if (score != 0) {
                ranked.push_back({g, d, score});
            }
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const Choice& a, const Choice& b) {
            return a.score > b.score;
        });
        std::set<std::vector<u8>> contexts;
        for (const auto& choice : ranked) {
            const auto context = MeshContext(impl->donors[choice.donor]);
            if (contexts.emplace(context.begin(), context.end()).second) {
                choices.push_back(choice);
                if (contexts.size() == 2) {
                    break;
                }
            }
        }
    }
    std::stable_sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) {
        return a.score > b.score;
    });
    size_t written = 0;
    for (const auto& choice : choices) {
        if (stop.stop_requested() || written >= impl->max_candidates) {
            break;
        }
        std::vector<MeshWarmStage> stages;
        for (const auto& stage : impl->variations[choice.group]) {
            stages.push_back(*stage);
        }
        const auto candidate = MakeMeshWarmCandidate(stages, impl->donors[choice.donor]);
        if (!candidate) {
            ++impl->skipped;
            continue;
        }
        GraphicsKey key;
        std::memcpy(key.data(), candidate->data() + candidate->size() - key.size(), key.size());
        if (!known.insert(key).second) {
            continue;
        }
        out.write(reinterpret_cast<const char*>(candidate->data()), candidate->size());
        if (!out) {
            break;
        }
        ++written;
    }
    return written;
}

size_t MeshWarmRecipes::TemplateCount() const { return impl->donors.size(); }
size_t MeshWarmRecipes::VariationCount() const { return impl->variations.size(); }
size_t MeshWarmRecipes::Skipped() const { return impl->skipped; }
} // namespace VideoCommon::ShaderWarm
