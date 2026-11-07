// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/offline_environment.h"
#include "shader_recompiler/offline_graphics.h"

namespace {
using namespace Shader;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class DeclaredEnvironment final : public Environment {
public:
    explicit DeclaredEnvironment(u32 count = 3) : pointer_count{count} { stage = Stage::Compute; }
    bool IsNativeProgram() const override { return true; }
    bool UsesStaticResourceLayout() const override { return true; }
    bool IsCbufStorageBufferPointer(u32 bank, u32 offset) override {
        return bank == 0 && offset >= 0x10 && offset < 0x10 + pointer_count * 16 && offset % 16 == 0;
    }
    u64 ReadInstruction(u32) override { throw std::runtime_error("unexpected instruction read"); }
    u32 ReadCbufValue(u32, u32) override { throw std::runtime_error("runtime constant read"); }
    TextureType ReadTextureType(u32) override { throw std::runtime_error("runtime texture read"); }
    TexturePixelFormat ReadTexturePixelFormat(u32) override { throw std::runtime_error("runtime format read"); }
    bool IsTexturePixelFormatInteger(u32) override { throw std::runtime_error("runtime format read"); }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return {}; }
    void Dump(u64, u64) override {}

private:
    u32 pointer_count;
};

void CheckMeshStorageTable(Stage stage, u32 base, u32 old_base) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    (void)ir.GetCbuf(ir.Imm32(0), ir.Imm32(base + 3 * 16));
    (void)ir.GetCbuf(ir.Imm32(0), ir.Imm32(old_base));
    (void)ir.LoadGlobal32(ir.Imm64(u64{0x12345678}));
    IR::Program program;
    program.stage = stage;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    OfflineEnvironment env;
    env.SetStage(stage);
    Optimization::CollectShaderInfoPass(env, program);
    Require(program.info.nvn_buffer_base == base, "mesh storage table uses a graphics slot alias");
    Require(program.info.nvn_buffer_used.count() == 1 && program.info.nvn_buffer_used.test(3),
            "mesh storage descriptor usage was lost or taken from the wrong table");
    Require(program.info.uses_global_memory, "test did not retain its global access");
    Optimization::AddNVNStorageBuffers(program);
    auto& descriptors = program.info.storage_buffers_descriptors;
    Require(descriptors.size() == 1 && descriptors[0].cbuf_index == 0 &&
            descriptors[0].cbuf_offset == base + 3 * 16,
            "global-memory fallback did not bind the mesh table slot");
    program.info.stores_global_memory = true;
    Optimization::AddNVNStorageBuffers(program);
    Require(descriptors.size() == 1 && descriptors[0].is_written,
            "fallback duplicated a descriptor or lost its write flag");
    descriptors.clear();
    for (size_t index = 0; index < descriptors.capacity(); ++index) {
        descriptors.push_back({.cbuf_index = 1, .cbuf_offset = static_cast<u32>(index * 16), .count = 1});
    }
    bool rejected = false;
    try { Optimization::AddNVNStorageBuffers(program); }
    catch (const Shader::NotImplementedException&) { rejected = true; }
    Require(rejected, "fallback exceeded the descriptor limit");
    descriptors[0] = {.cbuf_index = 0, .cbuf_offset = base + 3 * 16, .count = 1};
    Optimization::AddNVNStorageBuffers(program);
    Require(descriptors.size() == descriptors.capacity() && descriptors[0].is_written,
            "a full descriptor array could not reuse its existing mesh slot");
}

void CheckTextureContract(bool declared, u32 bank = 2, u32 offset = 0x20) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto handle = ir.GetCbuf(ir.Imm32(bank), ir.Imm32(offset));
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    (void)ir.ImageSampleExplicitLod(handle,
        ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)), ir.Imm32(0.0f), {}, flags);
    IR::Program program;
    program.stage = Stage::Fragment;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    OfflineEnvironment env;
    env.SetStage(Stage::Fragment);
    if (declared) env.SetTextureContract({TextureType::Color2D, TexturePixelFormat::A8B8G8R8_UNORM, false});
    HostTranslateInfo host{};
    host.ApplyDescriptorLimitPolicy();
    Optimization::TexturePass(env, program, host);
    Require(env.RequiredConstants().empty(), "texture contract synthesized a raw handle");
    Require(env.RequiredTextures().size() == 1 && program.info.texture_descriptors.size() == 1,
            "texture contract did not preserve its descriptor dependency");
    const auto& descriptor = program.info.texture_descriptors.front();
    Require(descriptor.cbuf_index == 2 && descriptor.cbuf_offset == 0x20,
            "texture contract changed the runtime binding location");
    bool handle_missing = false;
    try { env.ReadCbufValue(2, 0x20); }
    catch (const MissingOfflineContext&) { handle_missing = true; }
    Require(handle_missing, "texture contract fabricated a live handle");
    ConstBufferAddr dynamic{.index = 2, .offset = 0x20, .dynamic_offset = ir.Imm32(0U), .count = 1};
    Require(!env.DeclaredTextureSpecialization(dynamic), "texture contract accepted a capped dynamic array");
    ConstBufferAddr array{.index = 2, .offset = 0x20, .count = 2};
    Require(!env.DeclaredTextureSpecialization(array), "texture contract accepted an array");
    ConstBufferAddr computed{.index = 2, .offset = 0x20, .count = 1, .handle_program = 0};
    Require(!env.DeclaredTextureSpecialization(computed), "texture contract accepted a computed handle");
}

void CheckSecondaryTextureContracts() {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    for (const u32 offset : {0x568U, 0x570U}) {
        const auto primary = ir.GetCbuf(ir.Imm32(2U), ir.Imm32(0x20U));
        const auto secondary = ir.GetCbuf(ir.Imm32(2U), ir.Imm32(offset));
        const auto shifted = IR::U32{ir.ShiftLeftLogical(secondary, ir.Imm32(20U))};
        (void)ir.ImageSampleExplicitLod(ir.BitwiseOr(primary, shifted),
            ir.CompositeConstruct(ir.Imm32(0.0f), ir.Imm32(0.0f)), ir.Imm32(0.0f), {}, flags);
    }
    IR::Program program;
    program.stage = Stage::Fragment;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    OfflineEnvironment env;
    env.SetStage(Stage::Fragment);
    env.SetTextureContract({TextureType::Color2D, TexturePixelFormat::A8B8G8R8_UNORM, false});
    HostTranslateInfo host{};
    host.ApplyDescriptorLimitPolicy();
    Optimization::TexturePass(env, program, host);
    Require(env.RequiredConstants().empty() && env.RequiredTextures().size() == 2,
            "secondary sampler dependencies were merged by the texture cache");
    Require(env.RequiredTextures()[0].sec_offset != env.RequiredTextures()[1].sec_offset,
            "secondary sampler dependency lost its address");
}

std::vector<u32> Emit(u32 width, u32 alignment, bool int64 = true, bool literal_address = false,
                      bool indirect_words = false, u32 other_offset = 0x20,
                      bool immediate_branch = false) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto word = [&](u32 offset) { return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset)); };
    const auto select = ir.IEqual(word(0), ir.Imm32(0));
    IR::U32 other_low{word(other_offset)};
    IR::U32 other_high{word(other_offset + 4)};
    if (immediate_branch) {
        other_low = ir.Imm32(0x1000);
        other_high = ir.Imm32(0);
    }
    if (indirect_words) {
        const auto holder = ir.PackUint2x32(ir.CompositeConstruct(other_low, other_high));
        other_low = ir.LoadGlobal32(holder);
        other_high = ir.LoadGlobal32(ir.IAdd(holder, ir.Imm64(u64{4})));
    }
    const IR::U32 low{ir.Select(select, word(0x10), other_low)};
    const IR::U32 high{ir.Select(select, word(0x14), other_high)};
    const auto address = literal_address ? ir.Imm64(u64{0x1000})
        : ir.PackUint2x32(ir.CompositeConstruct(ir.IAdd(low, word(4)), high));
    IR::U32 value;
    if (width == 4) {
        value = ir.LoadGlobal32(address);
    } else if (width == 8) {
        value = IR::U32{ir.CompositeExtract(ir.LoadGlobal64(address), 0)};
    } else {
        value = IR::U32{ir.CompositeExtract(ir.LoadGlobal128(address), 0)};
    }
    ir.WriteGlobal32(ir.PackUint2x32(ir.CompositeConstruct(word(0x30), word(0x34))), value);

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    DeclaredEnvironment env;
    HostTranslateInfo host{};
    host.min_ssbo_alignment = alignment;
    host.support_int64 = int64;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    Require(program.info.uses_static_buffer_bounds, "static bounds were lost");
    Require(program.info.storage_buffers_descriptors.size() == 3, "candidate binding missing");
    Optimization::CollectShaderInfoPass(env, program);
    Profile profile{};
    profile.supported_spirv = 0x00010400;
    profile.support_int64 = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = alignment;
    auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    size_t lengths = 0;
    bool validity_offset = false;
    for (size_t i = 5; i < code.size();) {
        const u32 count = code[i] >> 16;
        Require(count && i + count <= code.size(), "invalid SPIR-V instruction extent");
        const auto op = code[i] & 0xffff;
        lengths += op == 68; // OpArrayLength
        if (op == 72 && count == 5 && code[i + 3] == 35 && code[i + 4] == 24) {
            validity_offset = true; // OpMemberDecorate Offset 24
        }
        i += count;
    }
    Require(lengths > 0 && validity_offset, "range or validity metadata missing");
    return code;
}

std::vector<u32> EmitHighBinding(u32 target, u32 device_limit = 64) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::IREmitter ir{block};
    const auto word = [&](u32 offset) { return ir.GetCbuf(ir.Imm32(0), ir.Imm32(offset)); };
    const auto offset = [](u32 index) { return 0x10 + index * 16; };
    const auto selector = word(0);
    IR::U32 low = word(offset(target));
    IR::U32 high = word(offset(target) + 4);
    for (u32 index = 1; index < target; ++index) {
        const auto selected = ir.IEqual(selector, ir.Imm32(index));
        low = IR::U32{ir.Select(selected, word(offset(index)), low)};
        high = IR::U32{ir.Select(selected, word(offset(index) + 4), high)};
    }
    const auto address = ir.PackUint2x32(ir.CompositeConstruct(low, high));
    const auto value = ir.LoadGlobal32(address);
    ir.WriteGlobal32(ir.PackUint2x32(ir.CompositeConstruct(word(offset(0)), word(offset(0) + 4))), value);
    ir.WriteGlobal32(ir.IAdd(address, ir.Imm64(u64{4})), ir.Imm32(0x13579bdfU));

    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    DeclaredEnvironment env{target + 1};
    HostTranslateInfo host{};
    host.min_ssbo_alignment = 16;
    host.support_int64 = true;
    host.max_per_stage_descriptor_storage_buffers = device_limit;
    host.max_descriptor_set_storage_buffers = device_limit;
    Optimization::GlobalMemoryToStorageBufferPass(env, program, host);
    Require(program.info.uses_static_buffer_bounds, "high binding lost static bounds");
    Require(program.info.storage_buffers_descriptors.size() == target + 1,
            "high binding test lost candidate descriptors");
    Require(program.info.storage_buffers_descriptors[target].cbuf_offset == offset(target),
            "target did not receive the requested high binding index");
    Optimization::CollectShaderInfoPass(env, program);
    Profile profile{};
    profile.supported_spirv = 0x00010400;
    profile.support_int64 = true;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = 16;
    return Backend::SPIRV::EmitSPIRV(profile, program);
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 2) {
        return 2;
    }
    const std::filesystem::path directory{argv[1]};
    std::filesystem::create_directories(directory);
    VaryingState input_loads{};
    input_loads.Set(IR::Attribute::Generic0X);
    input_loads.Set(IR::Attribute::Generic6X);
    std::array<u8, 32> input_types{};
    Require(!RequiredOfflineVertexTypes(input_loads, input_types), "missing vertex inputs accepted");
    input_types[0] = 1;
    input_types[6] = 3;
    Require(RequiredOfflineVertexTypes(input_loads, input_types) == (u64{1} | (u64{3} << 12)),
            "unsigned vertex type lost");
    input_types[6] = 2;
    Require(RequiredOfflineVertexTypes(input_loads, input_types) == (u64{1} | (u64{2} << 12)),
            "signed vertex type lost");
    input_types[6] = 3;
    std::array<AttributeType, 32> emitted_types{};
    Require(!MatchesOfflineVertexTypes(input_loads, input_types, emitted_types),
            "material integer type was accepted as a float input");
    emitted_types[6] = AttributeType::UnsignedInt;
    Require(MatchesOfflineVertexTypes(input_loads, input_types, emitted_types),
            "matching material integer input was rejected");
    input_types[6] = 4;
    Require(!RequiredOfflineVertexTypes(input_loads, input_types), "unknown vertex type accepted");
    CheckMeshStorageTable(Stage::Task, 0x670, 0x110);
    CheckMeshStorageTable(Stage::Mesh, 0x770, 0x310);
    CheckTextureContract(true);
    CheckSecondaryTextureContracts();
    bool missing_texture_context = false;
    try { CheckTextureContract(false); }
    catch (const MissingOfflineContext&) { missing_texture_context = true; }
    Require(missing_texture_context, "ordinary texture lookup used an undeclared contract");
    missing_texture_context = false;
    try { CheckTextureContract(true, 1, 0); }
    catch (const MissingOfflineContext&) { missing_texture_context = true; }
    Require(missing_texture_context, "texture contract was applied outside the C2 table");
    const std::array<Ampere::Instruction, 1> native{{{{0x794d, 0, 0x03800000, 0}}}};
    const auto lowered = Ampere::LowerToMaxwell(native);
    std::vector<u8> source(20);
    std::memcpy(source.data(), native.data(), 16);
    const u32 constant = 0x12345678;
    std::memcpy(source.data() + 16, &constant, 4);
    std::vector<u8> control(224);
    const auto put = [&](size_t offset, u32 value) { std::memcpy(control.data() + offset, &value, 4); };
    put(0, 0x98761234); put(4, 0xc9); put(56, 16); put(64, 16); put(68, 20);
    put(84, 5U << 16); put(88, 32); put(92, 1); put(96, 1); put(0xba, 16);
    OfflineEnvironment archive_env{source, control, lowered};
    Require(archive_env.WorkgroupSize() == std::array<u32, 3>{32, 1, 1}, "archive workgroup changed");
    Require(archive_env.ReadCbufValue(1, 0) == constant, "archive C1 word changed");
    Require(archive_env.RequiredConstants().size() == 1, "constant dependency was not retained");
    for (const u32 width : {4U, 8U, 16U}) {
        for (const u32 alignment : {4U, 16U}) {
            const auto code = Emit(width, alignment);
            std::ofstream output(directory / (std::to_string(width) + "-" + std::to_string(alignment) + ".spv"),
                                 std::ios::binary);
            output.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
            Require(bool(output), "cannot save SPIR-V test artifact");
        }
    }
    for (const u32 target : {32U, 63U}) {
        const auto code = EmitHighBinding(target);
        std::ofstream output(directory / ("high" + std::to_string(target) + ".spv"), std::ios::binary);
        output.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        Require(bool(output), "cannot save high-binding SPIR-V test artifact");
    }
    bool rejected = false;
    try { EmitHighBinding(63, 32); } catch (const NotImplementedException&) { rejected = true; }
    Require(rejected, "high binding ignored the device storage limit");
    rejected = false;
    try { Emit(4, 4, false); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "static mode accepted a host without int64");
    rejected = false;
    try { Emit(4, 4, true, true); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "static mode accepted an unclassified literal address");
    rejected = false;
    try { Emit(4, 4, true, false, true); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "static mode accepted a pointer loaded as two global words");
    rejected = false;
    try { Emit(4, 4, true, false, false, 0x24); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "static mode accepted an unclassified constant buffer branch");
    rejected = false;
    try { Emit(4, 4, true, false, false, 0x20, true); } catch (const std::exception&) { rejected = true; }
    Require(rejected, "static mode accepted an immediate pointer branch");
    std::cout << "Static candidate routing and SPIR-V range checks passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
