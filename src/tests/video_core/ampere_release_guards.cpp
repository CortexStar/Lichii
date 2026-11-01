// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_override.h"

TEST_CASE("Ampere ordinary gathers reject unproved half precision modes",
          "[video_core][ampere][release_guards]") {
    for (const u32 precision : {1U, 2U, 3U}) {
        const std::array<Shader::Ampere::Instruction, 2> code{{
            {{0x06027b63, 0x33c04407, 0x041e0f01U | (precision << 15), 0x000fc200}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        const auto lowered = Shader::Ampere::LowerToMaxwell(code);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native texture gather mode");
        REQUIRE(lowered.error_offset == 0);
        REQUIRE(lowered.instructions.empty());
        REQUIRE(lowered.source_instruction_offsets.empty());
    }
}

TEST_CASE("Ampere IADD3.X with a third source rejects carry outputs and a second carry input",
          "[video_core][ampere][release_guards]") {
    // IADD3.X R1, R1, RZ, R2, P0
    constexpr u32 plain = 0x007fe402;
    const auto lower = [](u32 word2) {
        const std::array<Shader::Ampere::Instruction, 2> code{{
            {{0x01017210, 0xff, word2, 0x000fc800}},
            {{0x0000794d, 0, 0x03800000, 0}},
        }};
        return Shader::Ampere::LowerToMaxwell(code);
    };
    REQUIRE(static_cast<bool>(lower(plain)));
    for (const u32 word2 : {(plain & ~(7U << 17)) | (1U << 17),    // carry out P1
                            (plain & ~(7U << 20)) | (1U << 20),    // second carry out P1
                            (plain & ~(15U << 13)) | (1U << 13)}) { // second carry in P1
        const auto lowered = lower(word2);
        REQUIRE_FALSE(static_cast<bool>(lowered));
        REQUIRE(lowered.error == "native integer carry mode");
    }
}

TEST_CASE("Ampere program overrides validate the surface buffer",
          "[video_core][ampere][release_guards]") {
    const std::array<Shader::Ampere::Instruction, 1> code{{{{0x0000794d, 0, 0x03800000, 0}}}};
    const auto lowered = Shader::Ampere::LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    auto bytes = Shader::Ampere::EncodeProgramOverride(code, lowered);
    REQUIRE(Shader::Ampere::ProgramOverrideSourceSize(bytes) == sizeof(code));
    for (const u32 surface : {3U, 18U}) {
        std::memcpy(bytes.data() + 6 * sizeof(u32), &surface, sizeof(surface));
        REQUIRE(static_cast<bool>(Shader::Ampere::DecodeProgramOverride(bytes, code)) == (surface < 18));
    }
}
