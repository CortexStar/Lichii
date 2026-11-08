// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdlib>
#include <tuple>
#include <utility>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/draw_class_stats.h"
#include "video_core/engines/maxwell_3d.h"

namespace VideoCommon::DrawClass {
namespace {

using Regs = Tegra::Engines::Maxwell3D::Regs;
static_assert(std::tuple_size_v<GroupTable> == Regs::NUM_REGS);

/// Maxwell 3D registers are 32-bit words: a field's bytes as a register index or count.
constexpr size_t RegWords(size_t bytes) {
    return bytes / 4;
}

#define DC_OFF(field) RegWords(offsetof(Regs, field))
#define DC_NUM(field) RegWords(sizeof(Regs::field))

void Fill(GroupTable& table, size_t begin, size_t count, Group group) {
    for (size_t i = begin; i < begin + count && i < table.size(); ++i) {
        table[i] = Bit(group);
    }
}

const char* const keylog_path = std::getenv("EDEN_NX2_DRAW_KEYLOG");

} // Anonymous namespace

void BuildGroupTable(GroupTable& table) {
    table.fill(Bit(Other));
    // Draw parameters.
    Fill(table, DC_OFF(draw), DC_NUM(draw), DrawParams);
    Fill(table, DC_OFF(vertex_buffer), DC_NUM(vertex_buffer), DrawParams);
    Fill(table, DC_OFF(index_buffer.first), 2, DrawParams);
    Fill(table, DC_OFF(index_buffer32_first), 6, DrawParams);
    Fill(table, DC_OFF(draw_inline_index), 1, DrawParams);
    Fill(table, DC_OFF(inline_index_2x16), DC_NUM(inline_index_2x16), DrawParams);
    Fill(table, DC_OFF(inline_index_4x8), DC_NUM(inline_index_4x8), DrawParams);
    Fill(table, DC_OFF(vertex_id_base), 1, DrawParams);
    Fill(table, DC_OFF(global_base_vertex_index), 1, DrawParams);
    Fill(table, DC_OFF(global_base_instance_index), 1, DrawParams);
    Fill(table, DC_OFF(draw_zero_index), 1, DrawParams);
    Fill(table, DC_OFF(draw_auto_start_byte_count), 1, DrawParams);
    // Constant buffers.
    Fill(table, DC_OFF(const_buffer), DC_NUM(const_buffer), ConstData);
    Fill(table, DC_OFF(bind_groups), DC_NUM(bind_groups), ConstBind);
    // Texture pools.
    Fill(table, DC_OFF(tex_header), DC_NUM(tex_header), TexturePools);
    Fill(table, DC_OFF(tex_sampler), DC_NUM(tex_sampler), TexturePools);
    Fill(table, DC_OFF(sampler_binding), DC_NUM(sampler_binding), TexturePools);
    // Render targets.
    Fill(table, DC_OFF(rt), DC_NUM(rt), RenderTargets);
    Fill(table, DC_OFF(zeta), DC_NUM(zeta), RenderTargets);
    Fill(table, DC_OFF(zeta_size), DC_NUM(zeta_size), RenderTargets);
    Fill(table, DC_OFF(zeta_enable), DC_NUM(zeta_enable), RenderTargets);
    Fill(table, DC_OFF(surface_clip), DC_NUM(surface_clip), RenderTargets);
    Fill(table, DC_OFF(rt_control), DC_NUM(rt_control), RenderTargets);
    // Shader programs.
    Fill(table, DC_OFF(pipelines), DC_NUM(pipelines), Shaders);
    // Vertex and index buffers.
    Fill(table, DC_OFF(vertex_streams), DC_NUM(vertex_streams), VertexIndex);
    Fill(table, DC_OFF(vertex_stream_limits), DC_NUM(vertex_stream_limits), VertexIndex);
    Fill(table, DC_OFF(vertex_stream_instances), DC_NUM(vertex_stream_instances), VertexIndex);
    Fill(table, DC_OFF(vertex_attrib_format), DC_NUM(vertex_attrib_format), VertexIndex);
    Fill(table, DC_OFF(index_buffer), DC_OFF(index_buffer.first) - DC_OFF(index_buffer),
         VertexIndex);
}

#undef DC_OFF
#undef DC_NUM

const GroupTable& Table() {
    static const GroupTable table = [] {
        GroupTable result{};
        BuildGroupTable(result);
        return result;
    }();
    return table;
}

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_DRAW_CLASSES");
        return (value != nullptr && value[0] != '\0' && value[0] != '0') ||
               (keylog_path != nullptr && keylog_path[0] != '\0');
    }();
    return enabled;
}

Outcome Classify(const DrawKey& previous, const DrawKey& current) {
    Outcome outcome{};
    outcome.continuous = (current.changes & ~CONTINUOUS_MASK) == 0;
    outcome.same_pipeline = current.pipeline != nullptr && current.pipeline == previous.pipeline;
    const bool hashes = current.bindings_valid && previous.bindings_valid;
    outcome.same_handles = outcome.continuous && outcome.same_pipeline && hashes &&
                           current.handle_hash == previous.handle_hash;
    outcome.memo_hit = outcome.same_handles && current.binding_hash == previous.binding_hash &&
                       current.render_targets_serial == previous.render_targets_serial &&
                       current.indexed == previous.indexed;
    return outcome;
}

Stats::Stats() : window_start{std::chrono::steady_clock::now()} {
    if (keylog_path != nullptr && keylog_path[0] != '\0') {
        keylog = std::fopen(keylog_path, "w");
        if (keylog == nullptr) {
            LOG_ERROR(HW_GPU, "EDEN_NX2_DRAW_KEYLOG: cannot open {}", keylog_path);
        } else {
            std::fputs("frame pipeline changes handles bindings rt_serial indexed first count "
                       "instances class\n",
                       keylog);
        }
    }
}

Stats::~Stats() {
    if (keylog != nullptr) {
        std::fclose(keylog);
    }
}

void Stats::NoteDraw(DrawKey key) {
    key.bindings_valid = std::exchange(pending_bindings, false);
    key.handle_hash = key.bindings_valid ? pending_handle_hash : 0;
    key.binding_hash = key.bindings_valid ? pending_binding_hash : 0;
    const Outcome outcome = has_previous ? Classify(previous, key) : Outcome{};
    ++draws;
    if (has_previous) {
        if (outcome.continuous) {
            ++continuous;
            if ((key.changes & ~Bit(DrawParams)) == 0) {
                ++draw_only;
            } else if ((key.changes & Bit(ConstBind)) == 0) {
                ++with_data;
            } else {
                ++with_binds;
            }
            continuous_same_pipeline += outcome.same_pipeline;
        } else {
            for (u32 group = 0; group < NumGroups; ++group) {
                if ((CONTINUOUS_MASK & (1U << group)) == 0 && (key.changes & (1U << group)) != 0) {
                    ++breaking[group];
                }
            }
        }
        same_pipeline += outcome.same_pipeline;
        same_handles += outcome.same_handles;
        memo_hits += outcome.memo_hit;
    }
    if (keylog != nullptr) {
        const char cls = outcome.memo_hit       ? 'M'
                         : outcome.same_handles ? 'H'
                         : outcome.continuous   ? 'C'
                                                : '-';
        fmt::print(keylog, "{} {} {:02x} {:016x} {:016x} {} {} {} {} {} {}\n", key.frame,
                   key.pipeline, key.changes, key.handle_hash, key.binding_hash,
                   key.render_targets_serial, key.indexed ? 1 : 0, key.first, key.count,
                   key.instances, cls);
    }
    previous = key;
    has_previous = true;
    if ((draws & 4095) == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start >= std::chrono::seconds{10}) {
            Report(now);
        }
    }
}

void Stats::Report(std::chrono::steady_clock::time_point now) {
    const double seconds = std::chrono::duration<double>(now - window_start).count();
    const auto pct = [this](u64 value) {
        return draws == 0 ? 0.0 : 100.0 * static_cast<double>(value) / static_cast<double>(draws);
    };
    LOG_INFO(HW_GPU,
             "Draw classes ({:.1f} s, {:.0f} draws/s): continuous {:.1f}% (draw parameters only "
             "{:.1f}%, + constant data {:.1f}%, + constant binds {:.1f}%; same pipeline {:.1f}%); "
             "breaking groups: texture pools {:.1f}%, render targets {:.1f}%, shaders {:.1f}%, "
             "vertex/index {:.1f}%, other {:.1f}%; same pipeline {:.1f}%; continuous with the "
             "previous draw's texture handles {:.1f}%, and its bindings and render targets "
             "(memo hits) {:.1f}%",
             seconds, static_cast<double>(draws) / seconds, pct(continuous), pct(draw_only),
             pct(with_data), pct(with_binds), pct(continuous_same_pipeline),
             pct(breaking[TexturePools]), pct(breaking[RenderTargets]), pct(breaking[Shaders]),
             pct(breaking[VertexIndex]), pct(breaking[Other]), pct(same_pipeline),
             pct(same_handles), pct(memo_hits));
    if (keylog != nullptr) {
        std::fflush(keylog);
    }
    draws = draw_only = with_data = with_binds = continuous = same_pipeline = 0;
    continuous_same_pipeline = same_handles = memo_hits = 0;
    breaking.fill(0);
    window_start = now;
}

Stats& GetStats() {
    static Stats stats;
    return stats;
}

} // namespace VideoCommon::DrawClass
