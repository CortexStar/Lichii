// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/draw_class_stats.h"
#include "video_core/engines/maxwell_3d.h"

namespace {
using namespace VideoCommon::DrawClass;
using Tegra::Engines::Maxwell3D;

u8 GroupOf(size_t method) {
    return Table()[method];
}

DrawKey Key(const void* pipeline, u8 changes) {
    return DrawKey{
        .pipeline = pipeline,
        .handle_hash = 1,
        .binding_hash = 2,
        .render_targets_serial = 3,
        .frame = 0,
        .first = 0,
        .count = 3,
        .instances = 1,
        .changes = changes,
        .indexed = true,
        .bindings_valid = true,
    };
}
} // Anonymous namespace

TEST_CASE("Draw class table groups the draw, constant buffer and binding registers",
          "[draw_class]") {
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(draw)) == Bit(DrawParams));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(vertex_buffer.count)) == Bit(DrawParams));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(index_buffer.first)) == Bit(DrawParams));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(index_buffer.count)) == Bit(DrawParams));
    // The index buffer's address and format are bindings, not draw parameters.
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(index_buffer.start_addr_low)) == Bit(VertexIndex));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(index_buffer.format)) == Bit(VertexIndex));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(const_buffer.offset)) == Bit(ConstData));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(const_buffer.buffer) + 15) == Bit(ConstData));
    // Every register of all five bind groups (0x20 bytes each), the last one included.
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(bind_groups)) == Bit(ConstBind));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(bind_groups) + sizeof(Maxwell3D::Regs::bind_groups) / 4 -
                    1) == Bit(ConstBind));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(bind_groups) + sizeof(Maxwell3D::Regs::bind_groups) / 4) !=
            Bit(ConstBind));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(tex_header.limit)) == Bit(TexturePools));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(sampler_binding)) == Bit(TexturePools));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(rt_control)) == Bit(RenderTargets));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(zeta_enable)) == Bit(RenderTargets));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(pipelines)) == Bit(Shaders));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(vertex_streams)) == Bit(VertexIndex));
    REQUIRE(GroupOf(MAXWELL3D_REG_INDEX(viewports)) == Bit(Other));
}

TEST_CASE("Draw class: only draw parameters and constants keep a draw continuous", "[draw_class]") {
    int pipeline{};
    const DrawKey previous = Key(&pipeline, 0);
    SECTION("constant data and binds") {
        const Outcome outcome =
            Classify(previous, Key(&pipeline, Bit(DrawParams) | Bit(ConstData) | Bit(ConstBind)));
        REQUIRE(outcome.continuous);
        REQUIRE(outcome.same_pipeline);
        REQUIRE(outcome.same_handles);
        REQUIRE(outcome.memo_hit);
    }
    SECTION("any other group ends the run") {
        for (const Group group : {TexturePools, RenderTargets, Shaders, VertexIndex, Other}) {
            const Outcome outcome = Classify(previous, Key(&pipeline, Bit(group)));
            REQUIRE_FALSE(outcome.continuous);
            REQUIRE_FALSE(outcome.memo_hit);
        }
    }
    SECTION("another pipeline, other handles or other bindings are no memo hit") {
        int other{};
        REQUIRE_FALSE(Classify(previous, Key(&other, 0)).same_pipeline);
        DrawKey handles = Key(&pipeline, 0);
        handles.handle_hash = 9;
        REQUIRE_FALSE(Classify(previous, handles).same_handles);
        DrawKey bindings = Key(&pipeline, 0);
        bindings.binding_hash = 9;
        const Outcome outcome = Classify(previous, bindings);
        REQUIRE(outcome.same_handles);
        REQUIRE_FALSE(outcome.memo_hit);
        DrawKey targets = Key(&pipeline, 0);
        targets.render_targets_serial = 9;
        REQUIRE_FALSE(Classify(previous, targets).memo_hit);
    }
    SECTION("draws whose bindings were not hashed never count as repeats") {
        DrawKey unhashed = Key(&pipeline, 0);
        unhashed.bindings_valid = false;
        REQUIRE_FALSE(Classify(previous, unhashed).same_handles);
    }
}
