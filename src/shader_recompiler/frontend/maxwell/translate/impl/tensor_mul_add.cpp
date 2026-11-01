// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {

void TranslatorVisitor::TENSOR_MUL_ADD(u64 insn) {
    union {
        u64 raw;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> matrix_a;
        BitField<20, 8, IR::Reg> matrix_b;
        BitField<39, 8, IR::Reg> matrix_c;
    } const tensor{insn};

    const IR::Reg ar = tensor.matrix_a;
    const IR::Reg br = tensor.matrix_b;
    const IR::Reg cr = tensor.matrix_c;
    const IR::Reg dr = tensor.dest_reg;

    const u64 ai = static_cast<u64>(ar);
    const u64 bi = static_cast<u64>(br);
    const u64 ci = static_cast<u64>(cr);
    const u64 di = static_cast<u64>(dr);
    const bool has_c = ci != 255;

    const IR::U32 a0 = X(ar);
    const IR::U32 a1 = X(IR::Reg{ai + 1});
    const IR::U32 a2 = X(IR::Reg{ai + 2});
    const IR::U32 a3 = X(IR::Reg{ai + 3});
    const IR::U32 b0 = X(br);
    const IR::U32 b1 = X(IR::Reg{bi + 1});
    const IR::U32 c0 = has_c ? X(cr) : ir.Imm32(0u);
    const IR::U32 c1 = has_c ? X(IR::Reg{ci + 1}) : ir.Imm32(0u);

    const IR::Value result{ir.Inst(IR::Opcode::TensorMulAdd, a0, a1, a2, a3, b0, b1, c0, c1)};
    X(dr, IR::U32{ir.CompositeExtract(result, 0)});
    X(IR::Reg{di + 1}, IR::U32{ir.CompositeExtract(result, 1)});
}

} // namespace Shader::Maxwell
