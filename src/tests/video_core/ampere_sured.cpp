// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/translate.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction captured{{0x0400759f, 0x60000c00, 0x0010e000, 0x000fe200}};
constexpr Instruction exit_instruction{{0x0000794d, 0, 0x03800000, 0}};

class SurfaceEnvironment final : public Shader::Environment {
public:
    SurfaceEnvironment(std::span<const u64> instructions_, u32 binding_, u32 offset_)
        : instructions{instructions_}, binding{binding_}, offset{offset_} {}

    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < instructions.size(); ++i) {
            if (address == 0x50 + i * 8 + ((i + 1) / 3) * 8) return instructions[i];
        }
        FAIL("Read outside lowered surface instructions");
        return 0;
    }
    u32 ReadCbufValue(u32 index, u32 byte_offset) override {
        REQUIRE(index == binding);
        REQUIRE(byte_offset == offset * 4);
        return 0x123;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        REQUIRE(handle == 0x123);
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        REQUIRE(handle == 0x123);
        return Shader::TexturePixelFormat::R32_UINT;
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        REQUIRE(handle == 0x123);
        return true;
    }
    u32 TextureBoundBuffer() const override { return binding; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {64, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}

private:
    std::span<const u64> instructions;
    u32 binding;
    u32 offset;
};

TEST_CASE("Ampere captured SURED lowers to a bound U32 image atomic add",
          "[video_core][ampere][sured]") {
    const u32 binding = GENERATE(0U, 2U, 17U);
    const u32 offset = GENERATE(12U, 0x1fffU);
    std::array<Instruction, 5> code{{
        {{0x00007802, 11, 0x00000f00, 0}},
        {{0x00047802, 7, 0x00000f00, 0}},
        {{0x00057802, 9, 0x00000f00, 0}},
        captured,
        exit_instruction,
    }};
    code[3].words[1] = 0x60000000 | (binding << 22) | (offset << 8);
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.required_texture_buffer == binding);
    REQUIRE(lowered.instructions[3] == (0xeb48000600070400ULL | (u64{offset} << 36)));

    SurfaceEnvironment env{lowered.instructions, binding, offset};
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::Program program;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    const u32 last = static_cast<u32>(lowered.instructions.size() - 1);
    Shader::Maxwell::Translate(env, &block, 0x50, 0x50 + last * 8 + ((last + 1) / 3) * 8);
    Shader::Optimization::SsaRewritePass(program);
    Shader::Optimization::ConstantPropagationPass(env, program);
    Shader::HostTranslateInfo host_info;
    host_info.ApplyDescriptorLimitPolicy();
    Shader::Optimization::TexturePass(env, program, host_info);
    Shader::Optimization::DeadCodeEliminationPass(program);
    const auto atomic = std::find_if(block.begin(), block.end(), [](const Shader::IR::Inst& inst) {
        return inst.GetOpcode() == Shader::IR::Opcode::ImageAtomicIAdd32;
    });
    REQUIRE(atomic != block.end());
    REQUIRE(atomic->MayHaveSideEffects());
    REQUIRE(atomic->Arg(0).U32() == 0);
    REQUIRE(atomic->Arg(2).U32() == 11);
    const auto* coordinates = atomic->Arg(1).InstRecursive();
    REQUIRE(coordinates->GetOpcode() == Shader::IR::Opcode::CompositeConstructU32x2);
    REQUIRE(coordinates->Arg(0).U32() == 7);
    REQUIRE(coordinates->Arg(1).U32() == 9);
    REQUIRE(program.info.image_descriptors.size() == 1);
    const auto& descriptor = program.info.image_descriptors.front();
    REQUIRE(descriptor.type == Shader::TextureType::Color2D);
    REQUIRE(descriptor.format == Shader::ImageFormat::R32_UINT);
    REQUIRE(descriptor.is_read);
    REQUIRE(descriptor.is_written);
    REQUIRE(descriptor.is_integer);
    REQUIRE(descriptor.cbuf_index == binding);
    REQUIRE(descriptor.cbuf_offset == offset * 4);
    REQUIRE(descriptor.count == 1);
}

TEST_CASE("Ampere SURED preserves predicate and register operands",
          "[video_core][ampere][sured]") {
    const u32 predicate = GENERATE(0U, 7U, 8U, 14U, 15U);
    const u32 data = GENERATE(0U, 32U, 245U, 255U);
    const u32 coordinate = GENERATE(0U, 4U, 244U);
    std::array code{captured, exit_instruction};
    code[0].words[0] = 0x59f | (predicate << 12) | (coordinate << 24);
    code[0].words[1] |= data;
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.instructions.size() == (predicate == 15 ? 1U : 2U));
    if (predicate != 15) {
        REQUIRE(lowered.instructions.front() ==
                (0xeb4800c600000000ULL | (u64{predicate} << 16) | (u64{coordinate} << 8) | data));
    }
}

TEST_CASE("Ampere SURED rejects unsupported formats and descriptor conflicts",
          "[video_core][ampere][sured]") {
    const u32 bit = GENERATE(59U, 60U, 61U, 62U, 63U, 72U, 73U, 74U, 75U,
                            77U, 78U, 79U, 80U, 84U, 85U, 86U, 87U, 88U, 89U);
    std::array code{captured, exit_instruction};
    code[0].words[bit / 32] ^= 1U << (bit % 32);
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    code[0] = captured;
    code[0].words[1] |= 1U << 21;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    code[0] = captured;
    code[0].words[1] |= 18U << 22;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    code[0] = captured;
    code[0].words[0] = 0xf500759f;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    code[0] = captured;
    code[0].words[1] |= 246;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(code)));
    std::array conflicting{captured, captured, exit_instruction};
    conflicting[1].words[1] |= 1U << 22;
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(conflicting)));
}
} // namespace
