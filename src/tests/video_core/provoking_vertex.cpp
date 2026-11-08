// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace {
using Vulkan::DynamicFeatures;
using Vulkan::FixedPipelineState;
using Maxwell = Tegra::Engines::Maxwell3D::Regs;

DynamicFeatures BothModes() {
    DynamicFeatures features{};
    features.has_provoking_vertex = true;
    features.has_provoking_vertex_first_mode = true;
    features.has_provoking_vertex_last_mode = true;
    return features;
}
} // Anonymous namespace

TEST_CASE("The guest's provoking vertex is used where the device has both modes",
          "[provoking_vertex]") {
    auto regs = std::make_unique<Maxwell>();
    const DynamicFeatures features = BothModes();
    regs->provoking_vertex = Maxwell::ProvokingVertex::Last;
    REQUIRE(FixedPipelineState::UseLastProvokingVertex(*regs, features));
    regs->provoking_vertex = Maxwell::ProvokingVertex::First;
    REQUIRE_FALSE(FixedPipelineState::UseLastProvokingVertex(*regs, features));
}

TEST_CASE("Transform feedback keeps the first vertex unless the device preserves the last",
          "[provoking_vertex]") {
    auto regs = std::make_unique<Maxwell>();
    regs->provoking_vertex = Maxwell::ProvokingVertex::Last;
    regs->transform_feedback_enabled = 1;
    DynamicFeatures features = BothModes();
    REQUIRE_FALSE(FixedPipelineState::UseLastProvokingVertex(*regs, features));
    features.has_provoking_vertex_tf_preserve = true;
    REQUIRE(FixedPipelineState::UseLastProvokingVertex(*regs, features));
}

TEST_CASE("A mode the device lacks falls back to the other one", "[provoking_vertex]") {
    auto regs = std::make_unique<Maxwell>();
    DynamicFeatures features = BothModes();
    features.has_provoking_vertex_last_mode = false;
    regs->provoking_vertex = Maxwell::ProvokingVertex::Last;
    REQUIRE_FALSE(FixedPipelineState::UseLastProvokingVertex(*regs, features));
    features = BothModes();
    features.has_provoking_vertex_first_mode = false;
    regs->provoking_vertex = Maxwell::ProvokingVertex::First;
    REQUIRE(FixedPipelineState::UseLastProvokingVertex(*regs, features));
    features.has_provoking_vertex = false;
    REQUIRE_FALSE(FixedPipelineState::UseLastProvokingVertex(*regs, features));
}
