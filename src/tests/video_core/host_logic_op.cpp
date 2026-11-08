// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host_logic_op.h"

namespace {
using Regs = Tegra::Engines::Maxwell3D::Regs;
using Type = Regs::VertexAttribute::Type;
using VideoCommon::HostLogicOpEnable;

std::unique_ptr<Regs> MakeRegs() {
    auto regs = std::make_unique<Regs>();
    for (auto& attrib : regs->vertex_attrib_format) {
        attrib.type.Assign(Type::UNorm);
    }
    return regs;
}

TEST_CASE("The host logic op follows the guest register", "[video_core]") {
    auto regs = MakeRegs();
    REQUIRE_FALSE(HostLogicOpEnable(*regs, false));
    REQUIRE_FALSE(HostLogicOpEnable(*regs, true));
    regs->logic_op.enable = 1;
    REQUIRE(HostLogicOpEnable(*regs, false));
    REQUIRE(HostLogicOpEnable(*regs, true));
}

TEST_CASE("Without the AMD workaround float attributes keep the logic op", "[video_core]") {
    auto regs = MakeRegs();
    regs->logic_op.enable = 1;
    regs->vertex_attrib_format[5].type.Assign(Type::Float);
    REQUIRE(HostLogicOpEnable(*regs, false));
}

TEST_CASE("The AMD workaround turns the logic op off only while an attribute is float",
          "[video_core]") {
    auto regs = MakeRegs();
    regs->logic_op.enable = 1;
    regs->vertex_attrib_format[31].type.Assign(Type::Float);
    REQUIRE_FALSE(HostLogicOpEnable(*regs, true));
    // The guest's register is untouched: once no attribute is float, the logic op is back on
    // without the guest writing it again.
    REQUIRE(regs->logic_op.enable == 1);
    regs->vertex_attrib_format[31].type.Assign(Type::SInt);
    REQUIRE(HostLogicOpEnable(*regs, true));
}

TEST_CASE("The AMD workaround never turns a disabled logic op on", "[video_core]") {
    auto regs = MakeRegs();
    REQUIRE_FALSE(HostLogicOpEnable(*regs, true));
    regs->vertex_attrib_format[0].type.Assign(Type::Float);
    REQUIRE_FALSE(HostLogicOpEnable(*regs, true));
}
} // namespace
