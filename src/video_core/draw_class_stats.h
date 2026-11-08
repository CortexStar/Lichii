// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdio>

#include "common/common_types.h"

// EDEN_NX2_DRAW_CLASSES=1: which register groups the guest changed between two graphics draws on
// the GPU thread (the split's back), and how many draws repeat the previous draw's pipeline,
// texture handles and resolved bindings. Logged at Info every 10 s. It sizes a "continuous draw"
// memo (Cemu's continuousDrawPass): the draws whose only changes are draw parameters and
// constant buffer data or binds.
// EDEN_NX2_DRAW_KEYLOG=<file>: also one line per draw with the keys behind those numbers.
namespace VideoCommon::DrawClass {

/// Register groups, one bit each in a draw's change mask.
enum Group : u8 {
    DrawParams,    ///< draw begin/end, first/count, base vertex/instance, inline indices
    ConstData,     ///< constant buffer selector and data words
    ConstBind,     ///< constant buffer binds (bind groups)
    TexturePools,  ///< TIC/TSC pool addresses and limits, sampler binding mode
    RenderTargets, ///< colour/depth targets, surface clip, target control
    Shaders,       ///< shader program registers
    VertexIndex,   ///< vertex streams, limits, attributes, index buffer address/format
    Other,         ///< every other register (fixed-function state, queries, ...)
    NumGroups,
};
static_assert(NumGroups <= 8, "a change mask is one byte");

constexpr u8 Bit(Group group) {
    return static_cast<u8>(1U << group);
}

/// The groups a continuous draw may change: everything else ends the run.
constexpr u8 CONTINUOUS_MASK = Bit(DrawParams) | Bit(ConstData) | Bit(ConstBind);

/// Maxwell 3D register index -> group bit.
using GroupTable = std::array<u8, 0xE00>;

/// The table every Maxwell3D uses (built once).
[[nodiscard]] const GroupTable& Table();

/// Fills `table` from the Maxwell 3D register layout.
void BuildGroupTable(GroupTable& table);

[[nodiscard]] bool Enabled();

/// What one draw repeated from the previous one.
struct DrawKey {
    const void* pipeline;
    u64 handle_hash;  ///< raw texture/sampler handles read from the constant buffers
    u64 binding_hash; ///< the image view and sampler ids they resolved to
    u64 render_targets_serial;
    u64 frame;
    u32 first;
    u32 count;
    u32 instances;
    u8 changes; ///< group bits changed since the previous draw
    bool indexed;
    bool bindings_valid; ///< the pipeline went through Configure (hashes are set)
};

/// The classification of one draw against the previous one.
struct Outcome {
    bool continuous;    ///< only CONTINUOUS_MASK groups changed
    bool same_pipeline; ///< and the same pipeline
    bool same_handles;  ///< and the same texture handles
    bool memo_hit;      ///< and the same bindings and render targets: a memo would replay it
};

[[nodiscard]] Outcome Classify(const DrawKey& previous, const DrawKey& current);

class Stats {
public:
    Stats();
    ~Stats();

    /// Called by the backend's Configure with the draw's binding hashes.
    void NoteBindings(u64 handle_hash, u64 binding_hash) {
        pending_handle_hash = handle_hash;
        pending_binding_hash = binding_hash;
        pending_bindings = true;
    }

    /// One graphics draw; `key` without hashes, taken from NoteBindings since the last draw.
    void NoteDraw(DrawKey key);

private:
    void Report(std::chrono::steady_clock::time_point now);

    DrawKey previous{};
    bool has_previous = false;
    u64 pending_handle_hash = 0;
    u64 pending_binding_hash = 0;
    bool pending_bindings = false;

    u64 draws = 0;
    u64 draw_only = 0;
    u64 with_data = 0;
    u64 with_binds = 0;
    u64 continuous = 0;
    u64 same_pipeline = 0;
    u64 continuous_same_pipeline = 0;
    u64 same_handles = 0;
    u64 memo_hits = 0;
    std::array<u64, NumGroups> breaking{}; ///< non-continuous draws that changed each group
    std::chrono::steady_clock::time_point window_start;

    std::FILE* keylog = nullptr;
};

[[nodiscard]] Stats& GetStats();

} // namespace VideoCommon::DrawClass
