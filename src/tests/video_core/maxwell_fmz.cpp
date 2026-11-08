// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
using IR::Opcode;

class FmzEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::logic_error("Unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { throw std::logic_error("Unexpected cbuf read"); }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

constexpr f32 INF = std::numeric_limits<f32>::infinity();
constexpr f32 NAN_F32 = std::numeric_limits<f32>::quiet_NaN();

constexpr u16 H_ZERO = 0x0000;
constexpr u16 H_QUARTER = 0x3400;
constexpr u16 H_HALF = 0x3800;
constexpr u16 H_THREE_QUARTERS = 0x3a00;
constexpr u16 H_ONE = 0x3c00;
constexpr u16 H_TWO = 0x4000;
constexpr u16 H_NINE_QUARTERS = 0x4080;
constexpr u16 H_INF = 0x7c00;

constexpr u32 Pack(u16 low, u16 high) {
    return u32{low} | u32{high} << 16;
}

f64 HalfValue(u16 bits) {
    const int exponent = (bits >> 10) & 31;
    const f64 fraction = bits & 1023;
    const f64 magnitude = exponent == 31
                              ? (fraction == 0 ? std::numeric_limits<f64>::infinity()
                                               : std::numeric_limits<f64>::quiet_NaN())
                              : std::ldexp(exponent == 0 ? fraction : fraction + 1024,
                                           exponent == 0 ? -24 : exponent - 25);
    return (bits & 0x8000) != 0 ? -magnitude : magnitude;
}

u16 HalfBits(f64 value) {
    for (u32 bits = 0; bits < 65536; ++bits) {
        const f64 half = HalfValue(static_cast<u16>(bits));
        if (half == value && std::signbit(half) == std::signbit(value)) {
            return static_cast<u16>(bits);
        }
        if (std::isnan(value) && std::isnan(half)) {
            return static_cast<u16>(bits);
        }
    }
    throw std::logic_error("Half result is not exact");
}

f64 Saturate(f64 value) {
    // Maxwell saturation maps NaN to zero.
    return std::isnan(value) ? 0.0 : std::clamp(value, 0.0, 1.0);
}

// Translates one instruction and interprets the resulting IR on a register file.
struct FmzProgram {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    FmzEnvironment env;
    Shader::Maxwell::TranslatorVisitor visitor{env, block};
    std::array<u32, IR::NUM_REGS> registers{};
    std::unordered_map<const IR::Inst*, u64> values;

    u64 Value(IR::Value value) const {
        if (!value.IsImmediate()) {
            return values.at(value.Inst());
        }
        switch (value.Type()) {
        case IR::Type::U1: return value.U1();
        case IR::Type::U32: return value.U32();
        case IR::Type::F32: return std::bit_cast<u32>(value.F32());
        default: throw std::logic_error("Unsupported immediate");
        }
    }

    f64 Float(IR::Value value) const {
        const u64 bits = Value(value);
        switch (value.Type()) {
        case IR::Type::F16: return HalfValue(static_cast<u16>(bits));
        case IR::Type::F32: return std::bit_cast<f32>(static_cast<u32>(bits));
        default: throw std::logic_error("Expected floating-point value");
        }
    }

    u64 Evaluate(const IR::Inst& inst) const {
        const auto a = [&](size_t index) { return Value(inst.Arg(index)); };
        const auto f = [&](size_t index) { return Float(inst.Arg(index)); };
        const auto fp = [&](f64 result) -> u64 {
            if (inst.Type() == IR::Type::F16) {
                return HalfBits(result);
            }
            return std::bit_cast<u32>(static_cast<f32>(result));
        };
        switch (inst.GetOpcode()) {
        case Opcode::GetRegister: return registers.at(static_cast<size_t>(inst.Arg(0).Reg()));
        case Opcode::BitCastF32U32:
        case Opcode::BitCastU32F32: return a(0);
        case Opcode::UnpackFloat2x16: return (a(0) & 0xffff) | (a(0) >> 16) << 32;
        case Opcode::PackFloat2x16: return (a(0) & 0xffff) | ((a(0) >> 32) & 0xffff) << 16;
        case Opcode::CompositeExtractF16x2: return (a(0) >> (a(1) * 32)) & 0xffff;
        case Opcode::CompositeConstructF16x2: return a(0) | a(1) << 32;
        case Opcode::ConvertF16F32: return fp(f(0));
        case Opcode::LogicalOr: return a(0) != 0 || a(1) != 0;
        case Opcode::SelectF16:
        case Opcode::SelectF32: return a(0) != 0 ? a(1) : a(2);
        case Opcode::FPOrdEqual16:
        case Opcode::FPOrdEqual32: return f(0) == f(1);
        case Opcode::FPUnordEqual16:
        case Opcode::FPUnordEqual32: return std::isnan(f(0)) || std::isnan(f(1)) || f(0) == f(1);
        case Opcode::FPMul16: return fp(f(0) * f(1));
        case Opcode::FPFma16: return fp(std::fma(f(0), f(1), f(2)));
        case Opcode::FPFma32: return fp(std::fma(f32(f(0)), f32(f(1)), f32(f(2))));
        case Opcode::FPSaturate16:
        case Opcode::FPSaturate32: return fp(Saturate(f(0)));
        default: throw std::logic_error(std::string{IR::NameOf(inst.GetOpcode())});
        }
    }

    u32 Run(u32 r1, u32 r2, u32 r3) {
        registers.fill(0xdeadbeefU);
        registers[1] = r1;
        registers[2] = r2;
        registers[3] = r3;
        values.clear();
        for (const auto& inst : block.Instructions()) {
            if (inst.GetOpcode() == Opcode::SetRegister) {
                registers.at(static_cast<size_t>(inst.Arg(0).Reg())) = u32(Value(inst.Arg(1)));
            } else {
                values.emplace(&inst, Evaluate(inst));
            }
        }
        return registers[0];
    }

    bool HasF16AfterLowering() {
        IR::Program program;
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        Shader::Optimization::LowerFp16ToFp32(program);
        return std::ranges::any_of(block.Instructions(), [](const IR::Inst& inst) {
            return inst.Type() == IR::Type::F16;
        });
    }
};

// FFMA R0, R1, R2, R3 with the FMZ field set to `fmz` (0 None, 1 FTZ, 2 FMZ).
u64 FfmaReg(u64 fmz, bool sat) {
    return 0x5980ULL << 48 | fmz << 53 | u64{sat} << 50 | 3ULL << 39 | 2ULL << 20 | 1ULL << 8;
}

// HFMA2 R0, R1, R2, R3 with H1_H0 swizzles and merge, so every operand stays F16.
u64 Hfma2Reg(u64 precision, bool sat) {
    return 0x5d00ULL << 48 | 3ULL << 39 | precision << 37 | u64{sat} << 32 | 2ULL << 20 |
           1ULL << 8;
}

// HMUL2 R0, R1, R2 with H1_H0 swizzles and merge, so both operands stay F16.
u64 Hmul2Reg(u64 precision, bool sat) {
    return 0x5d08ULL << 48 | precision << 39 | u64{sat} << 32 | 2ULL << 20 | 1ULL << 8;
}

u32 Bits(f32 value) {
    return std::bit_cast<u32>(value);
}

f32 RunFfma(u64 fmz, bool sat, f32 a, f32 b, f32 c) {
    FmzProgram program;
    program.visitor.FFMA_reg(FfmaReg(fmz, sat));
    return std::bit_cast<f32>(program.Run(Bits(a), Bits(b), Bits(c)));
}
} // namespace

TEST_CASE("Maxwell FFMA.FMZ keeps the addend when a multiplicand is zero",
          "[shader][maxwell][numeric][fmz]") {
    for (const bool sat : {false, true}) {
        INFO("sat " << sat);
        REQUIRE(RunFfma(2, sat, 0.0f, INF, 0.75f) == 0.75f);
        REQUIRE(RunFfma(2, sat, INF, 0.0f, 0.75f) == 0.75f);
        REQUIRE(RunFfma(2, sat, 0.0f, NAN_F32, 0.25f) == 0.25f);
        REQUIRE(RunFfma(2, sat, -0.0f, INF, 0.5f) == 0.5f);
        REQUIRE(RunFfma(2, sat, 0.5f, 0.5f, 0.25f) == 0.5f);
    }
    // Saturation still applies after the FMZ select.
    REQUIRE(RunFfma(2, true, 0.0f, INF, 2.0f) == 1.0f);
    REQUIRE(RunFfma(2, true, 0.0f, INF, -2.0f) == 0.0f);
    REQUIRE(RunFfma(2, true, 2.0f, 2.0f, 1.0f) == 1.0f);
    REQUIRE(RunFfma(2, false, 2.0f, 2.0f, 1.0f) == 5.0f);
}

TEST_CASE("Maxwell FFMA without FMZ is unchanged", "[shader][maxwell][numeric][fmz]") {
    for (const u64 mode : {0ULL, 1ULL}) {
        INFO("fmz mode " << mode);
        REQUIRE(std::isnan(RunFfma(mode, false, 0.0f, INF, 0.75f)));
        REQUIRE(RunFfma(mode, true, 0.0f, INF, 0.75f) == 0.0f);
        REQUIRE(RunFfma(mode, true, 0.5f, 0.5f, 0.25f) == 0.5f);
    }
}

TEST_CASE("Maxwell HFMA2.FMZ translates F16 operands and keeps the addend",
          "[shader][maxwell][numeric][fmz]") {
    for (const bool sat : {false, true}) {
        INFO("sat " << sat);
        FmzProgram program;
        REQUIRE_NOTHROW(program.visitor.HFMA2_reg(Hfma2Reg(2, sat)));
        REQUIRE(program.Run(Pack(H_ZERO, H_INF), Pack(H_INF, H_ZERO),
                            Pack(H_THREE_QUARTERS, H_QUARTER)) ==
                Pack(H_THREE_QUARTERS, H_QUARTER));
        REQUIRE(program.Run(Pack(H_HALF, H_TWO), Pack(H_HALF, H_ONE), Pack(H_QUARTER, H_QUARTER)) ==
                Pack(H_HALF, sat ? H_ONE : H_NINE_QUARTERS));
        REQUIRE_FALSE(program.HasF16AfterLowering());
    }
}

TEST_CASE("Maxwell HMUL2.FMZ translates F16 operands", "[shader][maxwell][numeric][fmz]") {
    for (const bool sat : {false, true}) {
        INFO("sat " << sat);
        FmzProgram program;
        REQUIRE_NOTHROW(program.visitor.HMUL2_reg(Hmul2Reg(2, sat)));
        REQUIRE(program.Run(Pack(H_ZERO, H_INF), Pack(H_INF, H_ZERO), 0) == Pack(H_ZERO, H_ZERO));
        REQUIRE(program.Run(Pack(H_HALF, H_TWO), Pack(H_HALF, H_ONE), 0) ==
                Pack(H_QUARTER, sat ? H_ONE : H_TWO));
        REQUIRE_FALSE(program.HasF16AfterLowering());
    }
}
