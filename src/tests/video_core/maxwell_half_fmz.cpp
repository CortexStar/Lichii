// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <span>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
namespace IR = Shader::IR;

u32 Address(u32 index) {
    return 0x50 + index * 8 + ((index + 1) / 3) * 8;
}

class FixtureEnvironment final : public Shader::Environment {
public:
    explicit FixtureEnvironment(std::span<const u64> code_) : code{code_} {
        stage = Shader::Stage::Compute;
    }

    u64 ReadInstruction(u32 address) override {
        for (u32 index = 0; index < code.size(); ++index) {
            if (Address(index) == address)
                return code[index];
        }
        FAIL("Read outside the instruction fixture at " << address);
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        FAIL("Unexpected constant buffer specialization");
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 4096;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {32, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    bool HasNativeInstructions() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> code;
};

// LDS R<reg>, [RZ + reg * 4] (32-bit), so the operands are not known at translation.
constexpr u64 LoadShared(u64 reg) {
    return 0xef48000000070000ULL | reg | (255ULL << 8) | ((reg * 4) << 20) | (4ULL << 48);
}
// R0 = R1 * {1.0, 1.0} with .FMZ, both operands packed halves (no F32 swizzle), then a 32-bit
// shared store of R0 and EXIT.
constexpr u64 HalfOne = 0x3c00 >> 6;
constexpr u64 Hmul2FmzImm =
    0x7800000000070000ULL | (1ULL << 8) | (HalfOne << 20) | (HalfOne << 30) | (2ULL << 39);
// R0 = R1 * R2 + R3 with .FMZ on packed halves.
constexpr u64 Hfma2FmzReg =
    0x5d00000000070000ULL | (1ULL << 8) | (2ULL << 20) | (3ULL << 39) | (2ULL << 37);
constexpr u64 StoreR0 = 0xef58000000070000ULL | (255ULL << 8) | (4ULL << 48);
constexpr u64 Exit = 0xe30000000007000fULL;

void TranslateAndEmit(std::span<const u64> code, bool float16) {
    FixtureEnvironment env{code};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<IR::Inst> inst_pool;
    Shader::ObjectPool<IR::Block> block_pool;
    Shader::HostTranslateInfo host_info{};
    host_info.support_float16 = float16;
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host_info);
    bool selects_zero = false;
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            selects_zero |= op == (float16 ? IR::Opcode::SelectF16 : IR::Opcode::SelectF32);
            CHECK((float16 || op != IR::Opcode::SelectF16));
        }
    }
    INFO(IR::DumpProgram(program));
    CHECK(selects_zero);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010400;
    const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    REQUIRE(spirv.size() > 5);
    CHECK(spirv.front() == 0x07230203);
}
} // namespace

TEST_CASE("Maxwell HMUL2 and HFMA2 .FMZ translate with half operands",
          "[video_core][maxwell][half]") {
    for (const u64 insn : {Hmul2FmzImm, Hfma2FmzReg}) {
        const std::array<u64, 6> code{LoadShared(1), LoadShared(2), LoadShared(3),
                                      insn,          StoreR0,       Exit};
        for (const bool float16 : {true, false}) {
            INFO("insn " << std::hex << insn << " float16 " << float16);
            TranslateAndEmit(code, float16);
        }
    }
}
