#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

// Every handle reads as an integer format, as a misresolved handle can.
class IntegerFormatEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::logic_error("Unexpected instruction read"); }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index == 0);
        return offset;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::R8_UINT;
    }
    bool IsTexturePixelFormatInteger(u32) override { return true; }
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
} // Anonymous namespace

TEST_CASE("Depth-compare textures are never integer", "[shader][depth_compare]") {
    Shader::ObjectPool<IR::Inst> pool{256};
    IR::Block block{pool};
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    IntegerFormatEnvironment env;
    IR::IREmitter ir{block};
    const auto coords{ir.CompositeConstruct(ir.Imm32(0.5f), ir.Imm32(0.5f))};

    IR::TextureInstInfo color_flags{};
    color_flags.type.Assign(Shader::TextureType::Color2D);
    const auto color{ir.ImageSampleImplicitLod(ir.GetCbuf(ir.Imm32(0), ir.Imm32(0)), coords,
                                               ir.Imm32(0.0f), {}, {}, color_flags)};

    IR::TextureInstInfo depth_flags{};
    depth_flags.type.Assign(Shader::TextureType::Color2D);
    depth_flags.is_depth.Assign(1);
    const auto depth{ir.ImageSampleDrefImplicitLod(ir.GetCbuf(ir.Imm32(0), ir.Imm32(4)), coords,
                                                   ir.Imm32(0.5f), ir.Imm32(0.0f), {}, {},
                                                   depth_flags)};

    Shader::HostTranslateInfo host;
    host.ApplyDescriptorLimitPolicy();
    Shader::Optimization::TexturePass(env, program, host);

    const auto& descriptors{program.info.texture_descriptors};
    REQUIRE(descriptors.size() == 2);
    const auto& color_desc{descriptors[color.Inst()->Flags<IR::TextureInstInfo>().descriptor_index]};
    const auto& depth_desc{descriptors[depth.Inst()->Flags<IR::TextureInstInfo>().descriptor_index]};
    // A plain sample still follows the descriptor's format.
    CHECK(color_desc.is_integer);
    // SPIR-V needs a float image for OpImageSampleDref*, whatever format the handle reports.
    CHECK(depth_desc.is_depth);
    CHECK_FALSE(depth_desc.is_integer);
    CHECK(depth.Inst()->Flags<IR::TextureInstInfo>().is_integer == 0);
}
