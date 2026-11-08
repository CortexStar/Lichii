// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <array>
#include <bit>
#include <climits>

#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/common/crypto/crc32.h"
#include "dynarmic/ir/microinstruction.h"

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;
namespace CRC32 = Common::Crypto::CRC32;

static void EmitCRC32Castagnoli(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, const int data_size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (code.HasHostFeature(HostFeature::SSE42)) {
        const Xbyak::Reg32 crc = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
        const Xbyak::Reg value = ctx.reg_alloc.UseGpr(code, args[1]).changeBit(data_size);
        if (data_size != 64) {
            code.crc32(crc, value);
        } else {
            code.crc32(crc.cvt64(), value);
        }
        ctx.reg_alloc.DefineValue(code, inst, crc);
    } else {
        ctx.reg_alloc.HostCall(code, inst, args[0], args[1], {});
        code.mov(code.ABI_PARAM3.cvt32(), data_size / CHAR_BIT); //zext
        code.CallFunction(&CRC32::ComputeCRC32Castagnoli);
    }
}

// Byte-at-a-time table of the reflected ISO polynomial: crc' = table[(crc ^ byte) & 0xFF] ^ (crc >> 8).
constexpr std::array<u32, 256> crc32_iso_table = [] {
    std::array<u32, 256> table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320U : 0U);
        }
        table[i] = crc;
    }
    return table;
}();

static void EmitCRC32ISO(BlockOfCode& code, EmitContext& ctx, IR::Inst* inst, const int data_size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (data_size < 32) {
        // CRC32B/CRC32H: one table lookup per byte. A loop over a buffer is a chain through crc,
        // and this step is a load and two ALU ops on it instead of the PCLMULQDQ reduction's
        // ~15-cycle chain.
        const Xbyak::Reg32 crc = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
        const Xbyak::Reg32 value = ctx.reg_alloc.UseScratchGpr(code, args[1]).cvt32();
        const Xbyak::Reg64 table = ctx.reg_alloc.ScratchGpr(code);
        const Xbyak::Reg64 index = ctx.reg_alloc.ScratchGpr(code);
        code.mov(table, std::bit_cast<u64>(crc32_iso_table.data()));
        if (data_size == 8) {
            code.movzx(value, value.cvt8());
        } else {
            code.movzx(value, value.cvt16());
        }
        // x = crc ^ data. A byte step on x leaves the next data byte in its low byte, so the
        // halfword is two steps of x' = table[x & 0xFF] ^ (x >> 8).
        code.xor_(crc, value);
        for (int step = 0; step < data_size / 8; ++step) {
            code.movzx(index.cvt32(), crc.cvt8());
            code.shr(crc, 8);
            code.xor_(crc, dword[table + index * 4]);
        }
        ctx.reg_alloc.DefineValue(code, inst, crc);
    } else if (code.HasHostFeature(HostFeature::PCLMULQDQ) && data_size == 32) {
        const Xbyak::Reg32 crc = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
        const Xbyak::Reg32 value = ctx.reg_alloc.UseGpr(code, args[1]).cvt32();
        const Xbyak::Xmm xmm_value = ctx.reg_alloc.ScratchXmm(code);
        const Xbyak::Xmm xmm_const = ctx.reg_alloc.ScratchXmm(code);

        code.movdqa(xmm_const, code.Const(xword, 0xb4e5b025'f7011641, 0x00000001'DB710641));

        code.xor_(crc, value);
        code.shl(crc.cvt64(), 32);
        code.movq(xmm_value, crc.cvt64());

        code.pclmulqdq(xmm_value, xmm_const, 0x00);
        code.pclmulqdq(xmm_value, xmm_const, 0x10);

        code.pextrd(crc, xmm_value, 2);

        ctx.reg_alloc.DefineValue(code, inst, crc);
    } else if (code.HasHostFeature(HostFeature::PCLMULQDQ) && data_size == 64) {
        const Xbyak::Reg32 crc = ctx.reg_alloc.UseScratchGpr(code, args[0]).cvt32();
        const Xbyak::Reg64 value = ctx.reg_alloc.UseGpr(code, args[1]);
        const Xbyak::Xmm xmm_value = ctx.reg_alloc.ScratchXmm(code);
        const Xbyak::Xmm xmm_const = ctx.reg_alloc.ScratchXmm(code);

        code.movdqa(xmm_const, code.Const(xword, 0xb4e5b025'f7011641, 0x00000001'DB710641));

        code.mov(crc, crc);
        code.xor_(crc.cvt64(), value);
        code.movq(xmm_value, crc.cvt64());

        code.pclmulqdq(xmm_value, xmm_const, 0x00);
        code.pclmulqdq(xmm_value, xmm_const, 0x10);

        code.pextrd(crc, xmm_value, 2);

        ctx.reg_alloc.DefineValue(code, inst, crc);
    } else {
        ctx.reg_alloc.HostCall(code, inst, args[0], args[1], {});
        code.mov(code.ABI_PARAM3, data_size / CHAR_BIT);
        code.CallFunction(&CRC32::ComputeCRC32ISO);
    }
}

void EmitX64::EmitCRC32Castagnoli8(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32Castagnoli(code, ctx, inst, 8);
}

void EmitX64::EmitCRC32Castagnoli16(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32Castagnoli(code, ctx, inst, 16);
}

void EmitX64::EmitCRC32Castagnoli32(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32Castagnoli(code, ctx, inst, 32);
}

void EmitX64::EmitCRC32Castagnoli64(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32Castagnoli(code, ctx, inst, 64);
}

void EmitX64::EmitCRC32ISO8(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32ISO(code, ctx, inst, 8);
}

void EmitX64::EmitCRC32ISO16(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32ISO(code, ctx, inst, 16);
}

void EmitX64::EmitCRC32ISO32(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32ISO(code, ctx, inst, 32);
}

void EmitX64::EmitCRC32ISO64(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32ISO(code, ctx, inst, 64);
}

}  // namespace Dynarmic::Backend::X64
