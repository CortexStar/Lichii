// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

constexpr u32 NUM_TEXTURE_SCALING_WORDS = 4;
constexpr u32 NUM_IMAGE_SCALING_WORDS = 2;
constexpr u32 NUM_TEXTURE_AND_IMAGE_SCALING_WORDS =
    NUM_TEXTURE_SCALING_WORDS + NUM_IMAGE_SCALING_WORDS;

struct RescalingLayout {
    alignas(16) std::array<u32, NUM_TEXTURE_SCALING_WORDS> rescaling_textures;
    alignas(16) std::array<u32, NUM_IMAGE_SCALING_WORDS> rescaling_images;
    u32 down_factor;
};
struct RenderAreaLayout {
    std::array<f32, 4> render_area;
};
/// Every push constant a pipeline's stages read, in one range. A stage reads its members from
/// one block (an entry point may use one push constant block), so the members never overlap: a
/// vertex shader's render area used to sit at offset 0, over the fragment shader's rescaling words.
/// The first words hold the device addresses of the tables shaders reach guest state through
/// (the same offsets as the push data of descriptor heap pipelines).
struct PushConstantLayout {
    u64 texture_table; ///< texture index table (descriptor heap pipelines)
    u64 bank_table;    ///< constant buffer bank addresses of the draw
    u64 page_table;    ///< VideoCommon::GpuPageTable of the channel's address space
    u64 reserved;
    RescalingLayout rescaling;
    RenderAreaLayout render_area;
};
constexpr u32 TEXTURE_TABLE_OFFSET = offsetof(PushConstantLayout, texture_table);
constexpr u32 BANK_TABLE_OFFSET = offsetof(PushConstantLayout, bank_table);
constexpr u32 PAGE_TABLE_OFFSET = offsetof(PushConstantLayout, page_table);
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET =
    offsetof(PushConstantLayout, rescaling) + offsetof(RescalingLayout, rescaling_textures);
constexpr u32 RESCALING_LAYOUT_IMAGES_OFFSET =
    offsetof(PushConstantLayout, rescaling) + offsetof(RescalingLayout, rescaling_images);
constexpr u32 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET =
    offsetof(PushConstantLayout, rescaling) + offsetof(RescalingLayout, down_factor);
constexpr u32 RENDERAREA_LAYOUT_OFFSET =
    offsetof(PushConstantLayout, render_area) + offsetof(RenderAreaLayout, render_area);
constexpr u32 PUSH_CONSTANT_LAYOUT_SIZE = sizeof(PushConstantLayout);

/// Constant buffers by device address (Info::uniforms_by_address): the draw's bank table holds one
/// entry per (stage, constant buffer), at (BankTableStage(stage) * BANK_TABLE_BANKS_PER_STAGE +
/// index) * sizeof(BankTableEntry). Reads at or past `size` return zero, as past a bound range.
struct BankTableEntry {
    u64 address; ///< device address of the bank's first byte (a zero page when unbound)
    u32 size;
    u32 reserved;
};
static_assert(sizeof(BankTableEntry) == 16);
constexpr u32 BANK_TABLE_BANKS_PER_STAGE = 18;
/// Graphics stages index the table by their pipeline slot; a dispatch has its own table.
[[nodiscard]] constexpr u32 BankTableStage(Stage stage) {
    return stage == Stage::Compute ? 0U : static_cast<u32>(stage);
}

/// The GPU page table as shaders read it (VideoCommon::GpuPageTable): L1 holds a u32 block index
/// per 2^PAGE_TABLE_BLOCK_BITS bytes of guest address space, the L2 blocks after it hold a u64
/// host device address per 2^PAGE_TABLE_PAGE_BITS bytes.
constexpr u32 PAGE_TABLE_ADDRESS_BITS = 40;
constexpr u32 PAGE_TABLE_PAGE_BITS = 12;
constexpr u32 PAGE_TABLE_BLOCK_BITS = 21;
constexpr u64 PAGE_TABLE_L1_BYTES =
    (u64{1} << (PAGE_TABLE_ADDRESS_BITS - PAGE_TABLE_BLOCK_BITS)) * sizeof(u32);
static_assert(RESCALING_LAYOUT_WORDS_OFFSET == 32 && RESCALING_LAYOUT_IMAGES_OFFSET == 48 &&
                  RESCALING_LAYOUT_DOWN_FACTOR_OFFSET == 56 && RENDERAREA_LAYOUT_OFFSET == 64 &&
                  PUSH_CONSTANT_LAYOUT_SIZE == 80,
              "push constant offsets are shared with the descriptor heap push data layout");

// Per-draw constants of Ocarina of Time's object motion, after every PushConstantLayout member.
struct OotMotionLayout {
    u32 current_base;
    u32 previous_base; // ~0 without history
    u32 vertex_bias;
    u32 vertex_range;
    u32 instance_bias;
    u32 flags; // bit 0: enabled for this draw
    u32 image_width; // pixels per row of the motion buffer
};
constexpr u32 OOT_MOTION_OFFSET = PUSH_CONSTANT_LAYOUT_SIZE;
constexpr u32 OOT_MOTION_PUSH_END = OOT_MOTION_OFFSET + 32;
static_assert(OOT_MOTION_OFFSET >= PUSH_CONSTANT_LAYOUT_SIZE);
static_assert(OOT_MOTION_OFFSET + sizeof(OotMotionLayout) <= OOT_MOTION_PUSH_END);

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info, IR::Program& program, Bindings& bindings);
[[nodiscard]] inline std::vector<u32> EmitSPIRV(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitSPIRV(profile, {}, program, binding);
}

/// Words per lane in the buffer of EmitCooperativeHmmaTest: the guest registers a0 a1 a2 a3 b0 b1
/// c0 c1 as input, then d0 d1 as output.
constexpr u32 COOPERATIVE_HMMA_TEST_LANE_WORDS = 10;

/// A compute shader of one warp running the HMMA.16816.F16 lowering on the registers in its storage
/// buffer (set 0, binding 0), to check at startup that the driver's matrix layout is the expected.
[[nodiscard]] std::vector<u32> EmitCooperativeHmmaTest(u32 spirv_version);

/// Mesh emulation without VK_EXT_mesh_shader. The mesh program runs as a compute shader
/// (RuntimeInfo::mesh_as_compute) that writes one block per workgroup to a u32 storage buffer:
/// max_vertices * vertex_lines words (vertex v, line l at v * vertex_lines + l), then
/// max_primitives * 3 vertex indices (0 0 0 past the primitive count, a degenerate triangle).
[[nodiscard]] inline u32 MeshEmulationBlockWords(const MeshInfo& mesh) {
    return mesh.max_vertices * static_cast<u32>(mesh.vertex_lines.size()) +
           mesh.max_primitives * 3;
}

/// Vertex shader drawing the emulation's output as a triangle list of
/// groups * max_primitives * 3 vertices, with the mesh program's vertex outputs. With a task
/// program (args_binding != ~0) the blocks start at the arguments' MESH_EMULATION_ARGS_BASE.
[[nodiscard]] std::vector<u32> EmitMeshEmulationVertex(const Profile& profile,
                                                       const MeshInfo& mesh, u32 binding,
                                                       u32 args_binding, bool convert_depth);

/// Mesh emulation with a task program: push constants of the distribution pass.
struct MeshEmulationDistributionConstants {
    u32 tasks;              ///< task workgroups of the draw
    u32 payload_words;      ///< words per task in the payload buffer (count at word 1)
    u32 capacity_words;     ///< words of the output arena
    u32 block_words;        ///< output words per mesh workgroup
    u32 vertices_per_group; ///< draw vertices per mesh workgroup
    u32 max_groups;         ///< the device's maxComputeWorkGroupCount[0]
};
/// Words of the distribution's argument buffer: vkCmdDispatchIndirect at 0, the mesh workgroups
/// the tasks asked for at 3, vkCmdDrawIndirect at MESH_EMULATION_DRAW_ARGS_OFFSET, then the
/// draw's first output word in the arena and its task count.
constexpr u32 MESH_EMULATION_ARGS_REQUESTED = 3;
constexpr u32 MESH_EMULATION_DRAW_ARGS_OFFSET = 16;
constexpr u32 MESH_EMULATION_ARGS_BASE = 8;
constexpr u32 MESH_EMULATION_ARGS_TASKS = 9;
constexpr u32 MESH_EMULATION_ARGS_WORDS = 10;

/// Distribution pass (one invocation) between the task and mesh dispatches, set 0: payload
/// buffer (binding 0), offsets (1: first mesh workgroup of each task), arguments (2) and the
/// arena counter (3: output words allocated so far, kept counting past the arena's end). The
/// draw's output is allocated from the arena every draw of the submission shares; mesh
/// workgroups that do not fit are not dispatched.
[[nodiscard]] std::vector<u32> EmitMeshEmulationDistribution(const Profile& profile);

} // namespace Shader::Backend::SPIRV
