// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <catch2/catch_test_macros.hpp>

#include "common/cityhash.h"
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/mesh_warm_builder.h"

namespace {
using namespace VideoCommon::ShaderWarm;

template <typename T>
void Put(std::vector<u8>& bytes, size_t offset, T value) {
    REQUIRE(offset + sizeof(T) <= bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
template <typename T>
T Get(std::span<const u8> bytes, size_t offset) {
    T value;
    REQUIRE(offset + sizeof(T) <= bytes.size());
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::vector<u8> Native(u32 stage, u32 tag) {
    std::vector<u8> code(144);
    Put(code, 0, (stage == 4 ? 5U : 1U) << 10);
    Put(code, 64, tag);
    Put(code, 128, 0x0000794dU); // EXIT PT
    Put(code, 136, 0x03800000U);
    return code;
}

MeshWarmStage Prepared(u32 stage, u32 tag) {
    const auto native = Native(stage, tag);
    LoweredCode lowered;
    REQUIRE_FALSE(LowerProgram(native, true, lowered));
    return {stage, StageHash(lowered.code), GraphicsEnv(stage, lowered, native)};
}

MeshWarmTemplate Donor(bool task = true) {
    MeshWarmTemplate donor;
    if (task) {
        donor.stages.push_back(Prepared(7, 1));
    }
    donor.stages.push_back(Prepared(8, 2));
    donor.stages.push_back(Prepared(4, 3));
    std::array<u32, 23> context{};
    context[0] = 0xfc404;
    context[1] = 0x2000000;
    context[2] = 0x802020;
    context[3] = 0x1234;
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(context.data()),
                                       sizeof(context)) | 1;
    for (size_t i = 0; i < donor.stages.size(); ++i) {
        auto& stage = donor.stages[i];
        if (stage.stage != 4) {
            const size_t at = stage.environment.size();
            stage.environment.resize(at + 92);
            std::memcpy(stage.environment.data() + at, context.data(),
                        stage.stage == 8 ? 92 : 12);
        }
        const size_t slot = stage.stage == 4 ? 5 : i == 0 ? 1 : 3;
        const u64 key = stage.code_hash ^ (stage.stage == 4 ? 0 : hash);
        std::memcpy(donor.key.data() + slot * 8, &key, 8);
    }
    return donor;
}

struct CacheFile {
    std::filesystem::path path;
    explicit CacheFile(std::span<const u8> record) {
        static std::atomic<u64> sequence{};
        path = std::filesystem::temp_directory_path() /
               ("lichii-mesh-warm-test-" + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence++) + ".bin");
        std::ofstream out(path, std::ios::binary);
        out.write("yuzucach", 8);
        const u32 version = WarmCacheVersion;
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        out.write(reinterpret_cast<const char*>(record.data()), record.size());
        REQUIRE(out.good());
    }
    ~CacheFile() { std::filesystem::remove(path); }
};

std::vector<u8> Bnsh() {
    std::vector<u8> bytes(0x1000);
    std::memcpy(bytes.data(), "BNSH", 4);
    bytes[8] = 1; bytes[9] = 4; bytes[10] = 2;
    bytes[12] = 0xff; bytes[13] = 0xfe;
    Put(bytes, 0x1c, static_cast<u32>(bytes.size()));
    std::memcpy(bytes.data() + 0x60, "grsc", 4);
    Put<u16>(bytes, 0x70, 4); Put<u16>(bytes, 0x72, 200);
    Put<u32>(bytes, 0x7c, 2); Put<u64>(bytes, 0x80, 0xa0);
    constexpr std::array<u8, 8> magic{0x34, 0x12, 0x76, 0x98, 0xc9, 0, 0, 0};
    for (size_t v = 0; v < 2; ++v) {
        const size_t program = 0x120 + v * 0x50;
        Put<u64>(bytes, 0xa0 + v * 0x40 + 0x10, program);
        Put<u64>(bytes, program, 2);
        for (size_t s = 0; s < 3; ++s) {
            const size_t index = v * 3 + s;
            const size_t record = 0x200 + index * 0x40;
            const size_t control = 0x400 + index * 0x60;
            const size_t code_at = 0x800 + index * 0x90;
            Put<u64>(bytes, program + (s == 0 ? 0x40 : s == 1 ? 0x48 : 0x28), record);
            Put<u64>(bytes, record + 8, control);
            Put<u64>(bytes, record + 16, code_at);
            Put<u32>(bytes, record + 24, 144); Put<u32>(bytes, record + 28, 88);
            std::memcpy(bytes.data() + control, magic.data(), magic.size());
            Put<u32>(bytes, control + 84, (s == 0 ? 6U : s == 1 ? 7U : 4U) << 16);
            const auto code = Native(s == 2 ? 4 : 8, static_cast<u32>(v + 1));
            std::memcpy(bytes.data() + code_at, code.data(), code.size());
        }
    }
    return bytes;
}
} // namespace

TEST_CASE("BNSH mesh variation boundaries preserve fragment and task pairing", "[mesh_warm]") {
    auto archive = Bnsh();
    size_t stages = 0, groups = 0;
    WalkArchive(archive, "test.bnsh", nullptr, [&](const auto&) { ++stages; },
                [](const auto&) { FAIL("unexpected member error"); }, [&](auto group) {
                    REQUIRE(group.size() == 3);
                    const auto tag = Get<u32>(group[0].code, 64);
                    for (const auto& stage : group) {
                        REQUIRE(Get<u32>(stage.code, 64) == tag);
                    }
                    REQUIRE(tag == ++groups);
                });
    REQUIRE(stages == 6);
    REQUIRE(groups == 2);
    // A stage pointing outside its own container cannot leak a partial candidate group.
    Put<u64>(archive, 0x200 + 16, archive.size() + 1);
    groups = 0;
    REQUIRE_THROWS_AS(WalkArchive(archive, "bad.bnsh", nullptr, [](const auto&) {},
                                  [](const auto&) {}, [&](auto) { ++groups; }), ArchiveError);
    REQUIRE(groups == 0);
}

TEST_CASE("Mesh warm records retain exact captures and native state hashes", "[mesh_warm]") {
    for (const bool task : {false, true}) {
        const auto donor = Donor(task);
        const auto record = MakeMeshWarmCandidate(donor.stages, donor);
        REQUIRE(record);
        CacheFile cache{*record};
        const auto loaded = ReadMeshWarmTemplates({cache.path, cache.path});
        REQUIRE(loaded.size() == 1);
        REQUIRE(loaded[0].key == donor.key);
        REQUIRE(loaded[0].stages.size() == donor.stages.size());
        for (size_t i = 0; i < donor.stages.size(); ++i) {
            REQUIRE(loaded[0].stages[i].environment == donor.stages[i].environment);
            REQUIRE(loaded[0].stages[i].code_hash == donor.stages[i].code_hash);
        }
        auto changed = donor.stages;
        changed[changed.size() - 2] = Prepared(8, 77);
        const auto predicted = MakeMeshWarmCandidate(changed, donor);
        REQUIRE(predicted);
        CacheFile predicted_cache{*predicted};
        const auto prediction = ReadMeshWarmTemplates({predicted_cache.path});
        REQUIRE(prediction.size() == 1);
        REQUIRE(prediction[0].key != donor.key);
        REQUIRE(prediction[0].stages.back().environment == donor.stages.back().environment);
        const auto& mesh = prediction[0].stages[prediction[0].stages.size() - 2].environment;
        const auto& original_mesh = donor.stages[donor.stages.size() - 2].environment;
        REQUIRE(std::equal(mesh.end() - 92, mesh.end(), original_mesh.end() - 92));
    }
}

TEST_CASE("Mesh candidate construction rejects malformed and mismatched recipes", "[mesh_warm]") {
    auto donor = Donor();
    const auto stages = donor.stages;
    donor.key[8] ^= 1;
    REQUIRE_FALSE(MakeMeshWarmCandidate(stages, donor));
    donor = Donor();
    donor.stages[0].environment.resize(12);
    REQUIRE_FALSE(MakeMeshWarmCandidate(stages, donor));
    donor = Donor();
    Put<u64>(donor.stages[0].environment, 8, ~0ULL);
    REQUIRE_FALSE(MakeMeshWarmCandidate(stages, donor));
    REQUIRE_FALSE(MakeMeshWarmCandidate(stages, Donor(false)));
    auto invalid = stages;
    std::swap(invalid[0], invalid[1]);
    REQUIRE_FALSE(MakeMeshWarmCandidate(invalid, Donor()));
    auto record = *MakeMeshWarmCandidate(stages, Donor());
    record.pop_back();
    CacheFile cache{record};
    REQUIRE(ReadMeshWarmTemplates({cache.path}).empty());
    CacheFile complete{*MakeMeshWarmCandidate(stages, Donor())};
    REQUIRE(ReadMeshWarmTemplates({cache.path, complete.path}).size() == 1);
    CacheFile partial_env{std::span{record}.first(83)};
    REQUIRE(ReadMeshWarmTemplates({partial_env.path, complete.path}).size() == 1);
    auto mismatched = Donor();
    mismatched.stages[0].environment[mismatched.stages[0].environment.size() - 92] ^= 1;
    REQUIRE_FALSE(MakeMeshWarmCandidate(stages, mismatched));
}

TEST_CASE("Mesh predictions deduplicate, respect caps and exclude known pipelines", "[mesh_warm]") {
    const auto donor = Donor();
    CacheFile cache{*MakeMeshWarmCandidate(donor.stages, donor)};
    const auto task = Native(7, 1), mesh = Native(8, 77), fragment = Native(4, 3);
    const std::array<ShaderRecord, 3> variation{{{ProgramStage::Task, task, {}},
                                               {ProgramStage::Mesh, mesh, {}},
                                               {ProgramStage::Fragment, fragment, {}}}};
    MeshWarmRecipes recipes{{cache.path}, 1};
    recipes.AddVariation(variation);
    recipes.AddVariation(variation);
    REQUIRE(recipes.TemplateCount() == 1);
    REQUIRE(recipes.VariationCount() == 1);
    const auto another_mesh = Native(8, 78);
    auto second = variation;
    second[1].code = another_mesh;
    recipes.AddVariation(second);
    REQUIRE(recipes.VariationCount() == 2);
    std::ostringstream out;
    REQUIRE(recipes.Write(out, {}) == 1);
    std::stop_source stopped;
    stopped.request_stop();
    std::ostringstream cancelled;
    REQUIRE(recipes.Write(cancelled, stopped.get_token()) == 0);
    REQUIRE(cancelled.str().empty());
    MeshWarmRecipes disabled{{cache.path}, 0};
    disabled.AddVariation(variation);
    REQUIRE(disabled.Write(out, {}) == 0);
    MeshWarmRecipes unknown{{}, 1};
    unknown.AddVariation(variation);
    REQUIRE(unknown.Write(out, {}) == 0);
    MeshWarmRecipes known{{cache.path}, 1};
    const auto original_mesh = Native(8, 2);
    auto existing = variation;
    existing[1].code = original_mesh;
    known.AddVariation(existing);
    REQUIRE(known.Write(out, {}) == 0);
    MeshWarmRecipes unrelated{{cache.path}, 1};
    const auto other_task = Native(7, 91), other_fragment = Native(4, 92);
    auto unsupported = variation;
    unsupported[0].code = other_task;
    unsupported[2].code = other_fragment;
    unrelated.AddVariation(unsupported);
    REQUIRE(unrelated.Write(out, {}) == 0);
}

TEST_CASE("Mesh warm variants preserve allocation-tail constant specialization", "[mesh_warm]") {
    const auto donor = Donor();
    CacheFile cache{*MakeMeshWarmCandidate(donor.stages, donor)};
    const auto task = Native(7, 1), fragment = Native(4, 3);
    auto mesh_a = Native(8, 2);
    mesh_a.resize(400);
    Put<u32>(mesh_a, 144, 0x12345678);
    auto mesh_b = mesh_a;
    Put<u32>(mesh_b, 144, 0x87654321);
    LoweredCode lowered_a, lowered_b;
    REQUIRE_FALSE(LowerProgram(mesh_a, true, lowered_a));
    REQUIRE_FALSE(LowerProgram(mesh_b, true, lowered_b));
    REQUIRE(lowered_a.code == lowered_b.code);
    REQUIRE(StageHash(lowered_a.code) == donor.stages[1].code_hash);
    auto stages = donor.stages;
    stages[1] = {8, StageHash(lowered_a.code), GraphicsEnv(8, lowered_a, mesh_a)};
    const auto candidate = MakeMeshWarmCandidate(stages, donor);
    REQUIRE(candidate);
    CacheFile predicted{*candidate};
    const auto loaded = ReadMeshWarmTemplates({predicted.path});
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].key == donor.key);
    const auto& environment = loaded[0].stages[1].environment;
    const size_t first_constant = 76 + Get<u64>(environment, 0);
    REQUIRE(Get<u32>(environment, first_constant + 8) == 0x12345678);
    REQUIRE(environment != donor.stages[1].environment);

    MeshWarmRecipes recipes{{cache.path}, 8};
    const std::array<ShaderRecord, 3> variation{{{ProgramStage::Task, task, {}},
                                                 {ProgramStage::Mesh, mesh_a, {}},
                                                 {ProgramStage::Fragment, fragment, {}}}};
    recipes.AddVariation(variation);
    auto second = variation;
    second[1].code = mesh_b;
    recipes.AddVariation(second);
    recipes.AddVariation(variation);
    REQUIRE(recipes.VariationCount() == 2);
    std::ostringstream out;
    REQUIRE(recipes.Write(out, {}) == 2);
}

TEST_CASE("Mesh warm prompts respect empty files and the per-game opt-in", "[mesh_warm]") {
    auto& enabled = Settings::values.use_mesh_shader_warmup;
    const bool was_global = enabled.UsingGlobal();
    const bool global_value = enabled.GetValue(true);
    enabled.SetGlobal(false);
    const bool custom_value = enabled.GetValue();
    SCOPE_EXIT {
        enabled.SetGlobal(false);
        enabled.SetValue(custom_value);
        enabled.SetGlobal(true);
        enabled.SetValue(global_value);
        enabled.SetGlobal(was_global);
    };
    enabled.SetGlobal(true);
    enabled.SetValue(true);
    CacheFile fixture{std::span<const u8>{}};
    const auto directory = std::filesystem::path{fixture.path.string() + ".dir"};
    std::filesystem::create_directories(directory);
    struct Restore {
        std::filesystem::path old_path, temporary;
        ~Restore() {
            Common::FS::SetEdenPath(Common::FS::EdenPath::ShaderDir, old_path);
            std::filesystem::remove_all(temporary);
        }
    } restore{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir), directory};
    Common::FS::SetEdenPath(Common::FS::EdenPath::ShaderDir, directory);
    constexpr u64 Title = 123;
    const auto mesh = MeshWarmPath(Title);
    std::filesystem::create_directories(mesh.parent_path());
    std::filesystem::copy_file(fixture.path, mesh);
    REQUIRE_FALSE(WarmPending(Title));
    {
        std::ofstream append(mesh, std::ios::app | std::ios::binary);
        append.put('x');
    }
    REQUIRE(WarmPending(Title));
    // A game can opt out while the global checkbox remains enabled.
    enabled.SetGlobal(false);
    enabled.SetValue(false);
    REQUIRE_FALSE(WarmPending(Title));
    REQUIRE(std::filesystem::exists(mesh));
    // Other warm-up stages still prompt when mesh prediction is disabled.
    const auto compute = ComputeWarmPath(Title);
    std::filesystem::copy_file(mesh, compute);
    REQUIRE(WarmPending(Title));
    std::filesystem::remove(compute);
    enabled.SetGlobal(true);
    REQUIRE(WarmPending(Title));
    {
        std::ofstream marker(WarmMarkerPath(mesh));
        marker << "done";
    }
    REQUIRE_FALSE(WarmPending(Title));
}
