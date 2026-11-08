// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

namespace VideoCommon {

/// How a render_enable condition (Maxwell3D::ProcessQueryCondition) is decided.
enum class ConditionalRendering : u8 {
    /// On the GPU (VK_EXT_conditional_rendering) or bypassed: the draws execute.
    Host,
    /// On the CPU, from guest memory as it stands: no cache holds a newer copy of the operands.
    GuestMemory,
    /// On the CPU, after flushing what the caches hold over the operands.
    Flush,
};

/// Which path a two-operand condition (IfEqual / IfNotEqual: 64-bit values at the address and
/// 16 bytes after it) takes in the Vulkan query cache, in the order it checks them.
enum class ConditionPath : u8 {
    ExtensionUnavailable, ///< No VK_EXT_conditional_rendering: the CPU decides after a flush
    Uncached,             ///< Neither operand is a query or GPU-written buffer data
    GuestSynchronized,    ///< No operand is GPU-written data or a query not yet in guest memory
    DriverBypass,         ///< A driver whose conditional rendering is skipped: always draw
    InitialZero,          ///< The first operand reads zero: a one-operand compare of the second
    CurrentZero,          ///< The second operand reads zero: a one-operand compare of the first
    AccuracyBypass,       ///< GPU accuracy below High: always draw
    QueryCacheBypass,     ///< Both operands are queries the cache holds: always draw
    ResolveCompare,       ///< Resolved on the GPU into the conditional rendering buffer
};

struct ConditionOperands {
    std::array<bool, 2> in_query_cache{}; ///< The query cache holds a query at the operand
    std::array<bool, 2> gpu_modified{};   ///< Not a query, and GPU-written in the buffer cache
    bool query_dirty{};                   ///< A host-managed query not synced to guest memory
};

/// The path QueryCacheRuntime::HostConditionalRenderingCompareValues takes. `reads_zero(i)` reads
/// operand i from guest memory; it is asked only for operands no cache holds, and only on the
/// paths that look at them.
template <typename ReadsZero>
[[nodiscard]] ConditionPath DecideConditionPath(bool extension, const ConditionOperands& operands,
                                                bool driver_bypass, bool gpu_level_high,
                                                ReadsZero&& reads_zero) {
    if (!extension) {
        return ConditionPath::ExtensionUnavailable;
    }
    std::array<bool, 2> cached{};
    for (size_t i = 0; i < 2; ++i) {
        cached[i] = operands.in_query_cache[i] || operands.gpu_modified[i];
    }
    if (!cached[0] && !cached[1]) {
        return ConditionPath::Uncached;
    }
    if (!operands.query_dirty && !operands.gpu_modified[0] && !operands.gpu_modified[1]) {
        return ConditionPath::GuestSynchronized;
    }
    if (driver_bypass) {
        return ConditionPath::DriverBypass;
    }
    for (size_t i = 0; i < 2; ++i) {
        if (!cached[i] && reads_zero(i)) {
            return i == 0 ? ConditionPath::InitialZero : ConditionPath::CurrentZero;
        }
    }
    if (!gpu_level_high) {
        return ConditionPath::AccuracyBypass;
    }
    if (!operands.gpu_modified[0] && !operands.gpu_modified[1]) {
        return ConditionPath::QueryCacheBypass;
    }
    return ConditionPath::ResolveCompare;
}

/// How Maxwell3D decides the condition after `path`. On the paths the query cache declines
/// (Uncached, GuestSynchronized) the CPU reads the 24-byte compare structure. A read through the
/// caches first downloads GPU-written texture and buffer data over it and writes the query
/// cache's pending results into it; `flush_writes` says whether any of that applies. When none
/// does, the CPU reads guest memory as it stands, without that flush (and the gathering of the
/// guest cores' CPU writes before it).
[[nodiscard]] constexpr ConditionalRendering HandlingOf(ConditionPath path, bool flush_writes) {
    switch (path) {
    case ConditionPath::ExtensionUnavailable:
        return ConditionalRendering::Flush;
    case ConditionPath::Uncached:
    case ConditionPath::GuestSynchronized:
        return flush_writes ? ConditionalRendering::Flush : ConditionalRendering::GuestMemory;
    default:
        return ConditionalRendering::Host;
    }
}

} // namespace VideoCommon
