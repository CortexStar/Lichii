// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/program_override.h"

namespace {
using namespace Shader::Ampere;

constexpr std::array<Instruction, 2> Source{
    {{{0x794d, 0, 0x03800000, 0}}, {{0x794d, 0, 0x03800000, 0}}}};
constexpr std::array<u64, 2> Canonical{0xe30000000007000fULL, 0xe30000000007000fULL};

std::vector<u8> LegacyFixture(u32 version) {
    const std::array<u32, 20> words{
        0x4e454445, version, 32,         2,          3,          version == 0x3230564f ? 2U : 0U,
        0x794d,     0,       0x03800000, 0,          0x794d,     0,
        0x03800000, 0,       0x0007000f, 0xe3000000, 0x0007000f, 0xe3000000,
        0,          1,
    };
    std::vector<u8> bytes((version == 0x3230564f ? 20 : 18) * sizeof(u32));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

void SetWord(std::vector<u8>& bytes, size_t index, u32 value) {
    std::memcpy(bytes.data() + index * sizeof(u32), &value, sizeof(value));
}

std::vector<u8> Fixture(u32 version) {
    if (version != 0x3330564f)
        return LegacyFixture(version);
    LoweredProgram program;
    program.instructions.assign(Canonical.begin(), Canonical.end());
    program.source_size = sizeof(Source);
    program.source_instruction_offsets = {0, 1};
    program.required_texture_buffer = 3;
    program.required_surface_buffer = 7;
    return EncodeProgramOverride(Source, program);
}
} // namespace

TEST_CASE("Ampere overrides decode historical six-word OV1 and OV2 layouts",
          "[video_core][ampere][override]") {
    for (const u32 version : {0x3130564fU, 0x3230564fU}) {
        const auto bytes = LegacyFixture(version);
        REQUIRE(ProgramOverrideSourceSize(bytes) == sizeof(Source));
        const auto program = DecodeProgramOverride(bytes, Source);
        REQUIRE(program);
        REQUIRE(program.source_size == sizeof(Source));
        REQUIRE(program.instructions == std::vector<u64>(Canonical.begin(), Canonical.end()));
        REQUIRE(program.required_texture_buffer == 3);
        REQUIRE(program.required_surface_buffer == ~0U);
        REQUIRE(program.source_instruction_offsets ==
                (version == 0x3230564f ? std::vector<u32>{0, 1} : std::vector<u32>{}));
    }
}

TEST_CASE("Ampere override versions reject truncation trailing bytes and wrong sources",
          "[video_core][ampere][override]") {
    for (const u32 version : {0x3130564fU, 0x3230564fU, 0x3330564fU}) {
        const auto bytes = Fixture(version);
        for (size_t size = 0; size < bytes.size(); ++size) {
            const std::span<const u8> truncated{bytes.data(), size};
            REQUIRE(ProgramOverrideSourceSize(truncated) == 0);
            REQUIRE_FALSE(DecodeProgramOverride(truncated, Source));
        }
        auto trailing = bytes;
        trailing.push_back(0);
        REQUIRE(ProgramOverrideSourceSize(trailing) == 0);
        REQUIRE_FALSE(DecodeProgramOverride(trailing, Source));
        auto changed_source = Source;
        changed_source[0].words[0] ^= 1;
        REQUIRE_FALSE(DecodeProgramOverride(bytes, changed_source));
        REQUIRE_FALSE(DecodeProgramOverride(bytes, std::span{Source}.first(1)));
        for (const auto& [index, value] : {std::pair<size_t, u32>{0, 0},
                                           {1, 0x3430564f},
                                           {2, 0},
                                           {2, 31},
                                           {2, 1048592},
                                           {3, 0},
                                           {3, 1048577},
                                           {5, 3}}) {
            auto invalid = bytes;
            SetWord(invalid, index, value);
            REQUIRE(ProgramOverrideSourceSize(invalid) == 0);
            REQUIRE_FALSE(DecodeProgramOverride(invalid, Source));
        }
    }
}

TEST_CASE("Ampere overrides preserve versioned maps and descriptor bank bounds",
          "[video_core][ampere][override]") {
    for (const u32 version : {0x3130564fU, 0x3230564fU, 0x3330564fU}) {
        for (const u32 texture : {0U, 17U, 18U, ~0U, ~0U - 1}) {
            auto bytes = Fixture(version);
            SetWord(bytes, 4, texture);
            const bool valid = texture < 18 || texture == ~0U;
            REQUIRE((ProgramOverrideSourceSize(bytes) != 0) == valid);
            REQUIRE(static_cast<bool>(DecodeProgramOverride(bytes, Source)) == valid);
        }
    }
    for (const u32 surface : {0U, 17U, 18U, ~0U, ~0U - 1}) {
        auto bytes = Fixture(0x3330564f);
        SetWord(bytes, 6, surface);
        const auto program = DecodeProgramOverride(bytes, Source);
        const bool valid = surface < 18 || surface == ~0U;
        REQUIRE((ProgramOverrideSourceSize(bytes) != 0) == valid);
        REQUIRE(static_cast<bool>(program) == valid);
        if (valid)
            REQUIRE(program.required_surface_buffer == surface);
    }
    auto unmapped = LegacyFixture(0x3130564f);
    SetWord(unmapped, 5, 2);
    REQUIRE(ProgramOverrideSourceSize(unmapped) == 0);
    for (const u32 version : {0x3230564fU, 0x3330564fU}) {
        const auto bytes = Fixture(version);
        const size_t map_word = bytes.size() / sizeof(u32) - 2;
        for (const auto& map : {std::array<u32, 2>{0, 2}, {1, 0}}) {
            auto invalid = bytes;
            SetWord(invalid, map_word, map[0]);
            SetWord(invalid, map_word + 1, map[1]);
            REQUIRE_FALSE(DecodeProgramOverride(invalid, Source));
        }
        auto absent_map = bytes;
        SetWord(absent_map, 5, 0);
        absent_map.resize(absent_map.size() - 8);
        REQUIRE(ProgramOverrideSourceSize(absent_map) == 0);
    }
}
