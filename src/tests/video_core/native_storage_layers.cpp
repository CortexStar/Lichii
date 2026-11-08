// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"

namespace {
using namespace Shader;

size_t Count(const std::vector<u32>& code, spv::Op opcode) {
    size_t count = 0;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        if ((code[offset] & 0xffffU) == static_cast<u32>(opcode)) {
            ++count;
        }
        offset += size;
    }
    return count;
}

/// A compute program with one storage buffer the storage buffer pass already lowered: its
/// pointer sits in constant buffer 1, which the program itself no longer reads.
struct Fixture {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile{};
    HostTranslateInfo host{};

    Fixture(bool storage, bool uniforms) {
        program.stage = Stage::Compute;
        program.workgroup_size = {1, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        profile.supported_spirv = 0x00010400;
        profile.support_int64 = true;
        profile.support_int64_atomics = true;
        profile.unified_descriptor_binding = true;
        profile.support_descriptor_aliasing = true;
        profile.min_ssbo_alignment = 16;
        profile.gpu_page_table = storage;
        profile.uniforms_by_address = uniforms;
        host.gpu_page_table = storage;
        host.uniforms_by_address = uniforms;
        auto& info = program.info;
        // Constant buffer 0 is read by the program; constant buffer 1 only holds the pointer.
        info.constant_buffer_descriptors = {{0, 1}};
        info.constant_buffer_mask = 1;
        info.constant_buffer_used_sizes[0] = 0x100;
        info.used_constant_buffer_types = IR::Type::U32;
        info.used_storage_buffer_types = IR::Type::U32;
        info.storage_buffers_descriptors.push_back(
            {.cbuf_index = 1, .cbuf_offset = 0x40, .count = 1, .is_written = true});
        IR::IREmitter ir{block};
        const IR::U32 value{ir.GetCbuf(ir.Imm32(0), ir.Imm32(0x10))};
        block.AppendNewInst(IR::Opcode::WriteStorage32, {ir.Imm32(0), ir.Imm32(0x20), value});
        Maxwell::MarkStorageByAddress(program, host);
    }
};

} // namespace

TEST_CASE("Storage through the page table binds the constant buffer holding its pointer",
          "[native_layers]") {
    for (const bool uniforms : {false, true}) {
        Fixture fixture{true, uniforms};
        const auto& info = fixture.program.info;
        REQUIRE(info.storage_by_address);
        REQUIRE(info.uniforms_by_address == uniforms);
        // The shader reads the pointer itself: constant buffer 1 is bound through its two words.
        REQUIRE((info.constant_buffer_mask & 2U) != 0);
        REQUIRE(info.constant_buffer_descriptors.size() == 2);
        REQUIRE(info.constant_buffer_descriptors[1].index == 1);
        REQUIRE(info.constant_buffer_used_sizes[1] >= 0x48);
        const auto code = Backend::SPIRV::EmitSPIRV(fixture.profile, fixture.program);
        // No storage buffer descriptors; with uniforms off, both banks are uniform buffers (one
        // aliased variable per bank and type read).
        size_t blocks = 0;
        for (size_t offset = 5; offset < code.size();) {
            const u32 size = code[offset] >> 16;
            if ((code[offset] & 0xffffU) == static_cast<u32>(spv::Op::OpVariable) &&
                code[offset + 3] == static_cast<u32>(spv::StorageClass::Uniform)) {
                ++blocks;
            }
            offset += size;
        }
        REQUIRE(blocks == (uniforms ? 0U : 4U));
        REQUIRE(Count(code, spv::Op::OpConvertUToPtr) != 0);
    }
}

TEST_CASE("Each native layer turned off on its own still emits its program", "[native_layers]") {
    for (const auto [storage, uniforms] : {std::pair{false, false}, std::pair{false, true},
                                           std::pair{true, false}, std::pair{true, true}}) {
        Fixture fixture{storage, uniforms};
        REQUIRE(fixture.program.info.storage_by_address == storage);
        REQUIRE(fixture.program.info.uniforms_by_address == uniforms);
        const auto code = Backend::SPIRV::EmitSPIRV(fixture.profile, fixture.program);
        REQUIRE(code.size() > 5);
        // Device addresses appear exactly when a layer uses them.
        REQUIRE((Count(code, spv::Op::OpConvertUToPtr) != 0) == (storage || uniforms));
    }
}
