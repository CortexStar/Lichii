// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "common/common_types.h"

namespace Shader::Backend::SPIRV {
namespace {

// HMMA.16816.F16 fragment layout (PTX m16n8k16), lane t = 4g + i:
//   A (16x16 row-major): a0={A[g][2i],A[g][2i+1]} a1={A[g+8][2i],..}
//                        a2={A[g][8+2i],..}       a3={A[g+8][8+2i],..}
//   B (16x8  row-major): b0={B[2i][g],B[2i+1][g]}  b1={B[8+2i][g],B[8+2i+1][g]}
//   C/D (16x8 row-major): d0={D[g][2i],D[g][2i+1]} d1={D[g+8][2i],D[g+8][2i+1]}
// The fragments are staged through a Workgroup f16 scratch and consumed with the
// NV cooperative-matrix instructions.
constexpr u32 A_BASE = 0;    // 16x16 f16
constexpr u32 B_BASE = 256;  // 16x8 f16
constexpr u32 C_BASE = 384;  // 16x8 f16
constexpr u32 D_BASE = 512;  // 16x8 f16

} // Anonymous namespace

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a0, Id a1, Id a2, Id a3, Id b0, Id b1,
                    Id c0, Id c1) {
    const Id u32ty{ctx.U32[1]};
    const Id f16{ctx.F16[1]};
    const Id f16x2{ctx.F16[2]};

    const auto add = [&](Id x, Id y) { return ctx.OpIAdd(u32ty, x, y); };
    const auto shl = [&](Id x, Id y) { return ctx.OpShiftLeftLogical(u32ty, x, y); };
    const auto shr = [&](Id x, Id y) { return ctx.OpShiftRightLogical(u32ty, x, y); };
    const auto and_ = [&](Id x, Id y) { return ctx.OpBitwiseAnd(u32ty, x, y); };
    const auto imm = [&](u32 v) { return ctx.Const(v); };

    const auto ptr = [&](Id index) {
        return ctx.uses_explicit_workgroup_layout
                   ? ctx.OpAccessChain(ctx.coop_scratch_f16, ctx.coop_scratch, imm(0U), index)
                   : ctx.OpAccessChain(ctx.coop_scratch_f16, ctx.coop_scratch, index);
    };
    const auto lo_f16 = [&](Id value) {
        return ctx.OpCompositeExtract(f16, ctx.OpBitcast(f16x2, value), 0U);
    };
    const auto hi_f16 = [&](Id value) {
        return ctx.OpCompositeExtract(f16, ctx.OpBitcast(f16x2, value), 1U);
    };
    const auto store = [&](Id index, Id value_f16) { ctx.OpStore(ptr(index), value_f16); };
    const auto load = [&](Id index) { return ctx.OpLoad(f16, ptr(index)); };
    const auto combine = [&](Id lo, Id hi) {
        return ctx.OpBitcast(u32ty, ctx.OpCompositeConstruct(f16x2, lo, hi));
    };
    const auto barrier = [&]() {
        ctx.OpControlBarrier(imm(3U), imm(3U), imm(0x108U));
    };

    const Id lane{ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id)};
    const Id sub_base{ctx.OpIMul(u32ty, ctx.OpLoad(u32ty, ctx.subgroup_id), imm(640U))};
    const Id abase{add(imm(A_BASE), sub_base)};
    const Id bbase{add(imm(B_BASE), sub_base)};
    const Id cbase{add(imm(C_BASE), sub_base)};
    const Id dbase{add(imm(D_BASE), sub_base)};
    const Id g{shr(lane, imm(2U))};   // lane / 4
    const Id i{and_(lane, imm(3U))};  // lane % 4
    const Id i2{shl(i, imm(1U))};     // 2i
    const Id i4_2{shl(add(i, imm(4U)), imm(1U))};  // 8+2i
    const Id g16{shl(g, imm(4U))};                 // g*16
    const Id g8_16{shl(add(g, imm(8U)), imm(4U))}; // (g+8)*16
    const Id g3{shl(g, imm(3U))};                  // g*8
    const Id g8_3{shl(add(g, imm(8U)), imm(3U))};  // (g+8)*8

    barrier();
    // Stage A (row-major, 16x16).
    {
        const Id idx0{add(abase, add(g16, i2))};
        const Id idx1{add(abase, add(g8_16, i2))};
        const Id idx2{add(abase, add(g16, i4_2))};
        const Id idx3{add(abase, add(g8_16, i4_2))};
        store(idx0, lo_f16(a0));
        store(add(idx0, imm(1U)), hi_f16(a0));
        store(idx1, lo_f16(a1));
        store(add(idx1, imm(1U)), hi_f16(a1));
        store(idx2, lo_f16(a2));
        store(add(idx2, imm(1U)), hi_f16(a2));
        store(idx3, lo_f16(a3));
        store(add(idx3, imm(1U)), hi_f16(a3));
    }
    // Stage B (row-major, 16x8): b0={B[2i][g],B[2i+1][g]}, b1={B[8+2i][g],B[8+2i+1][g]}.
    {
        const Id idx0{add(bbase, add(shl(i, imm(4U)), g))};  // (2i)*8+g
        const Id idx1{add(bbase, add(shl(add(i, imm(4U)), imm(4U)), g))}; // (8+2i)*8+g
        store(idx0, lo_f16(b0));
        store(add(idx0, imm(8U)), hi_f16(b0));
        store(idx1, lo_f16(b1));
        store(add(idx1, imm(8U)), hi_f16(b1));
    }
    // Stage C (row-major, 16x8).
    {
        const Id idx0{add(cbase, add(g3, i2))};
        const Id idx1{add(cbase, add(g8_3, i2))};
        store(idx0, lo_f16(c0));
        store(add(idx0, imm(1U)), hi_f16(c0));
        store(idx1, lo_f16(c1));
        store(add(idx1, imm(1U)), hi_f16(c1));
    }
    barrier();

    if (!Sirit::ValidId(ctx.cooperative_matrix_type_a)) {
        ctx.cooperative_matrix_type_a =
            ctx.TypeCooperativeMatrixNV(f16, imm(3U), imm(16U), imm(16U)); // Subgroup scope
    }
    if (!Sirit::ValidId(ctx.cooperative_matrix_type_b)) {
        ctx.cooperative_matrix_type_b =
            ctx.TypeCooperativeMatrixNV(f16, imm(3U), imm(16U), imm(8U));
    }
    ctx.cooperative_matrix_type_c = ctx.cooperative_matrix_type_b;

    const Id mat_a =
        ctx.OpCoopMatrixLoadNV(ctx.cooperative_matrix_type_a, ptr(abase), imm(16U),
                               ctx.false_value);
    const Id mat_b =
        ctx.OpCoopMatrixLoadNV(ctx.cooperative_matrix_type_b, ptr(bbase), imm(8U),
                               ctx.false_value);
    const Id mat_c =
        ctx.OpCoopMatrixLoadNV(ctx.cooperative_matrix_type_c, ptr(cbase), imm(8U),
                               ctx.false_value);
    const Id mat_d{ctx.OpCoopMatrixMulAddNV(ctx.cooperative_matrix_type_c, mat_a, mat_b, mat_c)};
    ctx.OpCoopMatrixStoreNV(ptr(dbase), mat_d, imm(8U), ctx.false_value);
    barrier();

    const Id d0_idx{add(dbase, add(g3, i2))};
    const Id d1_idx{add(dbase, add(g8_3, i2))};
    const Id d0{combine(load(d0_idx), load(add(d0_idx, imm(1U))))};
    const Id d1{combine(load(d1_idx), load(add(d1_idx, imm(1U))))};
    return ctx.OpCompositeConstruct(ctx.U32[2], d0, d1);
}

} // namespace Shader::Backend::SPIRV
