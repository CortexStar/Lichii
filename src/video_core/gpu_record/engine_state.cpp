// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <type_traits>

#include <fmt/format.h>

#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu_record/engine_state.h"
#include "video_core/gpu_record/record_format.h"
#include "video_core/macro.h"
#include "video_core/native_qmd.h"

namespace VideoCommon::GpuRecord {

namespace {

constexpr u32 ChannelBlobVersion = 1;
constexpr u32 SharedBlobVersion = 2;
constexpr u32 NoEngine = 0xffffffff;

template <typename T>
void WriteRaw(ByteWriter& w, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    w.U64(sizeof(T));
    w.Raw(&value, sizeof(T));
}

/// Reads what WriteRaw wrote; a size this build's type doesn't have is an error.
template <typename T>
bool ReadRaw(ByteReader& r, T& value, const char* name, std::string& error) {
    static_assert(std::is_trivially_copyable_v<T>);
    const u64 size = r.U64();
    if (!r.Ok() || size != sizeof(T)) {
        error = fmt::format("{}: {} bytes recorded, {} in this build", name, size, sizeof(T));
        return false;
    }
    r.Raw(&value, sizeof(T));
    if (!r.Ok()) {
        error = fmt::format("{}: the recording ends inside its {} bytes", name, sizeof(T));
        return false;
    }
    return true;
}

void WriteMacroEngine(ByteWriter& w, const Tegra::MacroEngine& engine) {
    w.U8(engine.native_turing ? 1 : 0);
    w.U32(engine.native_write_offset);
    WriteRaw(w, engine.native_code);
}

bool ReadMacroEngine(ByteReader& r, Tegra::MacroEngine& engine, std::string& error) {
    const bool native = r.U8() != 0;
    const u32 offset = r.U32();
    if (!ReadRaw(r, engine.native_code, "macro code", error)) {
        return false;
    }
    if (native != engine.native_turing) {
        error = "the recording's macro engine mode differs from this channel's";
        return false;
    }
    engine.native_write_offset = offset;
    // Compiled programs and memoized calls belong to the old code.
    ++engine.native_code_generation;
    engine.macro_cache.clear();
    return true;
}

} // Anonymous namespace

struct StateAccess {
    using Maxwell3D = Tegra::Engines::Maxwell3D;
    using KeplerCompute = Tegra::Engines::KeplerCompute;

    static std::vector<u8> Capture(Tegra::Control::ChannelState& channel) {
        auto& payload = *channel.payload;
        ByteWriter w;
        w.U32(ChannelBlobVersion);

        // DMA pusher and puller.
        auto& pusher = payload.dma_pusher;
        w.U64(pusher.suspensions.load(std::memory_order_acquire));
        for (size_t i = 0; i < pusher.subchannels.size(); ++i) {
            w.U32(pusher.subchannels[i] != nullptr ? static_cast<u32>(pusher.subchannel_type[i])
                                                   : NoEngine);
        }
        WriteRaw(w, pusher.puller.regs);
        WriteRaw(w, pusher.puller.bound_engines);

        // 3D engine.
        Maxwell3D& m = payload.maxwell_3d;
        WriteRaw(w, m.regs);
        WriteRaw(w, m.shadow_state);
        w.U64(m.state.shader_stages.size());
        for (const auto& stage : m.state.shader_stages) {
            w.U64(stage.const_buffers.size());
            for (const auto& cb : stage.const_buffers) {
                w.U64(cb.address);
                w.U32(cb.size);
                w.U8(cb.enabled ? 1 : 0);
            }
        }
        WriteRaw(w, m.root_tables.tables);
        w.U32(m.root_tables.selected);
        w.U32(m.root_tables.offset);
        w.U8(m.root_tables.loaded ? 1 : 0);
        WriteRaw(w, m.native_data_ram);
        WriteRaw(w, m.macro_positions);
        w.U8(m.execute_on ? 1 : 0);

        // Compute engine.
        KeplerCompute& c = payload.kepler_compute;
        WriteRaw(w, c.regs);
        WriteRaw(w, c.native_registers);
        w.U32(c.native_bind_pointer);
        w.U8(c.native_compute_uploaded ? 1 : 0);

        // The others hold nothing outside their registers between command lists.
        WriteRaw(w, payload.fermi_2d.regs);
        WriteRaw(w, payload.maxwell_dma.regs);
        WriteRaw(w, payload.kepler_memory.regs);
        return std::move(w.bytes);
    }

    static bool Restore(Tegra::Control::ChannelState& channel, std::span<const u8> blob,
                        std::string& error) {
        auto& payload = *channel.payload;
        ByteReader r{blob};
        if (r.U32() != ChannelBlobVersion) {
            error = "unknown channel state version";
            return false;
        }
        auto& pusher = payload.dma_pusher;
        pusher.suspensions.store(r.U64(), std::memory_order_release);
        for (u32 i = 0; i < pusher.subchannels.size(); ++i) {
            const u32 type = r.U32();
            if (type == NoEngine) {
                pusher.subchannels[i] = nullptr;
                continue;
            }
            Tegra::Engines::EngineInterface* engine = nullptr;
            switch (static_cast<Tegra::Engines::EngineTypes>(type)) {
            case Tegra::Engines::EngineTypes::Maxwell3D:
                engine = &payload.maxwell_3d;
                break;
            case Tegra::Engines::EngineTypes::KeplerCompute:
                engine = &payload.kepler_compute;
                break;
            case Tegra::Engines::EngineTypes::Fermi2D:
                engine = &payload.fermi_2d;
                break;
            case Tegra::Engines::EngineTypes::MaxwellDMA:
                engine = &payload.maxwell_dma;
                break;
            case Tegra::Engines::EngineTypes::KeplerMemory:
                engine = &payload.kepler_memory;
                break;
            case Tegra::Engines::EngineTypes::Nv01Timer:
                engine = &payload.nv01_timer;
                break;
            default:
                error = fmt::format("subchannel {}: unknown engine type {}", i, type);
                return false;
            }
            pusher.BindSubchannel(engine, i, static_cast<Tegra::Engines::EngineTypes>(type));
        }
        if (!ReadRaw(r, pusher.puller.regs, "puller registers", error) ||
            !ReadRaw(r, pusher.puller.bound_engines, "puller bindings", error)) {
            return false;
        }

        Maxwell3D& m = payload.maxwell_3d;
        if (!ReadRaw(r, m.regs, "3D registers", error) ||
            !ReadRaw(r, m.shadow_state, "3D shadow RAM", error)) {
            return false;
        }
        if (r.U64() != m.state.shader_stages.size()) {
            error = "shader stage count differs";
            return false;
        }
        for (auto& stage : m.state.shader_stages) {
            if (r.U64() != stage.const_buffers.size()) {
                error = "constant buffer count differs";
                return false;
            }
            for (auto& cb : stage.const_buffers) {
                cb.address = r.U64();
                cb.size = r.U32();
                cb.enabled = r.U8() != 0;
            }
        }
        if (!ReadRaw(r, m.root_tables.tables, "root tables", error)) {
            return false;
        }
        m.root_tables.selected = r.U32();
        m.root_tables.offset = r.U32();
        m.root_tables.loaded = r.U8() != 0;
        if (!ReadRaw(r, m.native_data_ram, "MME data RAM", error) ||
            !ReadRaw(r, m.macro_positions, "macro positions", error)) {
            return false;
        }
        m.execute_on = r.U8() != 0;
        // Every register changed as far as the rasterizer knows.
        m.dirty.flags.set();

        KeplerCompute& c = payload.kepler_compute;
        if (!ReadRaw(r, c.regs, "compute registers", error) ||
            !ReadRaw(r, c.native_registers, "compute native registers", error)) {
            return false;
        }
        c.native_bind_pointer = r.U32();
        c.native_compute_uploaded = r.U8() != 0;
        if (c.native_context != nullptr) {
            // As KeplerCompute::ProcessNativeMethod selects them once compute code was uploaded.
            c.native_macro = c.native_compute_uploaded ? &c.native_context->compute_engine
                                                       : &c.native_context->engine;
            c.native_macro_positions = c.native_compute_uploaded
                                           ? std::span<u32>{c.native_context->compute_positions}
                                           : std::span<u32>{c.native_context->positions};
        }

        if (!ReadRaw(r, payload.fermi_2d.regs, "2D registers", error) ||
            !ReadRaw(r, payload.maxwell_dma.regs, "DMA registers", error) ||
            !ReadRaw(r, payload.kepler_memory.regs, "inline-to-memory registers", error)) {
            return false;
        }
        if (!r.Ok() || !r.AtEnd()) {
            error = "channel state has the wrong size";
            return false;
        }
        return true;
    }

    static std::vector<u8> CaptureShared(Tegra::Control::ChannelState& channel) {
        ByteWriter w;
        w.U32(SharedBlobVersion);
        const auto& context = channel.payload->maxwell_3d.native_macro_context;
        w.U8(context != nullptr ? 1 : 0);
        if (context != nullptr) {
            WriteMacroEngine(w, context->engine);
            WriteRaw(w, context->positions);
            WriteMacroEngine(w, context->compute_engine);
            WriteRaw(w, context->compute_positions);
        }
        // QMDs scheduled or counted down but not run yet; finished ones are history.
        std::vector<std::pair<u64, const Tegra::NativeQmdScheduler::State*>> live;
        for (const auto& [address, state] : Tegra::NativeQmdScheduler::shared_states) {
            if (!state.completed || state.deferred != 0) {
                live.emplace_back(address, &state);
            }
        }
        w.U64(live.size());
        for (const auto& [address, state] : live) {
            w.U64(address);
            w.U32(static_cast<u32>(state->dependencies));
            w.U32(static_cast<u32>(state->deferred));
            w.U8((state->loaded ? 1 : 0) | (state->scheduled ? 2 : 0) | (state->completed ? 4 : 0));
        }
        return std::move(w.bytes);
    }

    static bool RestoreShared(Tegra::Control::ChannelState& channel, std::span<const u8> blob,
                              std::string& error) {
        ByteReader r{blob};
        if (r.U32() != SharedBlobVersion) {
            error = "unknown shared state version";
            return false;
        }
        const auto& context = channel.payload->maxwell_3d.native_macro_context;
        const bool recorded = r.U8() != 0;
        if (recorded != (context != nullptr)) {
            error = "the recording's macro context differs from this channel's (Switch 2 mode?)";
            return false;
        }
        if (context != nullptr) {
            if (!ReadMacroEngine(r, context->engine, error) ||
                !ReadRaw(r, context->positions, "macro positions", error) ||
                !ReadMacroEngine(r, context->compute_engine, error) ||
                !ReadRaw(r, context->compute_positions, "compute macro positions", error)) {
                return false;
            }
        }
        auto& states = Tegra::NativeQmdScheduler::shared_states;
        states.clear();
        const u64 count = r.U64();
        for (u64 i = 0; i < count && r.Ok(); ++i) {
            const u64 address = r.U64();
            Tegra::NativeQmdScheduler::State state{};
            state.dependencies = static_cast<s32>(r.U32());
            state.deferred = static_cast<s32>(r.U32());
            const u8 flags = r.U8();
            state.loaded = (flags & 1) != 0;
            state.scheduled = (flags & 2) != 0;
            state.completed = (flags & 4) != 0;
            states.emplace(address, state);
        }
        if (!r.Ok() || !r.AtEnd()) {
            error = "shared state has the wrong size";
            return false;
        }
        return true;
    }
};

std::vector<u8> CaptureChannel(Tegra::Control::ChannelState& channel) {
    return StateAccess::Capture(channel);
}

bool RestoreChannel(Tegra::Control::ChannelState& channel, std::span<const u8> blob,
                    std::string& error) {
    return StateAccess::Restore(channel, blob, error);
}

std::vector<u8> CaptureSharedState(Tegra::Control::ChannelState& channel) {
    return StateAccess::CaptureShared(channel);
}

bool RestoreSharedState(Tegra::Control::ChannelState& channel, std::span<const u8> blob,
                        std::string& error) {
    return StateAccess::RestoreShared(channel, blob, error);
}

} // namespace VideoCommon::GpuRecord
