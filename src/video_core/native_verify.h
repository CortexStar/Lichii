// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <string_view>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_NATIVE_VERIFY=<layer>[,<layer>...]: each native GPU layer (notes/native-gpu-backend.md)
// checks what it hands the GPU against the path it replaces. Every 10 s a layer with checks logs
// one Info line (checks, differences); its first 64 differences are logged as errors.
namespace VideoCommon::NativeVerify {

enum class Layer : u32 {
    Storage,    ///< GPU page table entries vs the buffers storage bindings resolve to
    Uniforms,   ///< constant buffer bank bytes the shader reads vs guest memory
    Textures,   ///< resident texture pool entries vs a full resolve of every resident hit
    Bindless,   ///< the pool-mirroring descriptor heap vs the bound pool entries
    Barriers,   ///< render pass barriers from a draw's remembered image set vs the full check
    DynState,   ///< dynamic state the command buffer holds vs the values the registers give
    Conditions, ///< render_enable operands read as guest memory stands vs read after a flush
    Count,
};

inline constexpr std::array<std::string_view, static_cast<size_t>(Layer::Count)> layer_names{
    "storage", "uniforms", "textures", "bindless", "barriers", "dynstate", "conditions",
};

/// The layers EDEN_NX2_NATIVE_VERIFY names, read once.
[[nodiscard]] u32 EnabledMask();

[[nodiscard]] inline bool Enabled(Layer layer) {
    static const u32 mask = EnabledMask();
    return ((mask >> static_cast<u32>(layer)) & 1U) != 0;
}

/// Parses an EDEN_NX2_NATIVE_VERIFY value (kept apart for the tests).
[[nodiscard]] u32 ParseMask(std::string_view value);

struct Counters {
    std::atomic<u64> checks{};
    std::atomic<u64> differences{};
    std::atomic<u64> logged{};
};

[[nodiscard]] Counters& CountersOf(Layer layer);

/// One check: logs the first 64 differences and the 10 s line.
template <typename... Args>
void Check(Layer layer, bool same, fmt::format_string<Args...> format, Args&&... args) {
    Counters& counters = CountersOf(layer);
    counters.checks.fetch_add(1, std::memory_order_relaxed);
    if (!same) {
        counters.differences.fetch_add(1, std::memory_order_relaxed);
        if (counters.logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU, "Native verify {}: {}", layer_names[static_cast<size_t>(layer)],
                      fmt::format(format, std::forward<Args>(args)...));
        }
    }
}

/// Resident texture pool entries against a full resolve of every resident hit.
[[nodiscard]] inline bool Textures() {
    return Enabled(Layer::Textures);
}

/// The pool-mirroring descriptor heap against the bound pool entries.
[[nodiscard]] inline bool Bindless() {
    return Enabled(Layer::Bindless);
}

/// Render pass barriers decided from a draw's remembered image set against the full per-draw
/// check (RenderPassBarrierTracker).
[[nodiscard]] inline bool Barriers() {
    return Enabled(Layer::Barriers);
}

/// Whether the device runs `layer` at all (the GPU page table needs buffer device addresses, ...).
/// An enabled layer that is off says so in its line instead of reporting 0 checks silently.
void SetActive(Layer layer, bool active);

/// Logs the line of each enabled layer counted through Check (storage, uniforms) once 10 s
/// passed since the last one, 0 checks included. Any thread.
void Report();

} // namespace VideoCommon::NativeVerify
