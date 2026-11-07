// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon {

/// Per-pipeline binding cache: a draw or dispatch whose texture handles, descriptor pool entries
/// and storage buffer bindings match the pipeline's previous one reuses the resolved views,
/// samplers, buffers and image descriptors. EDEN_NX2_BINDCACHE=0 resolves everything every time.
[[nodiscard]] inline bool BindCacheEnabled() noexcept {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_BINDCACHE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

} // namespace VideoCommon
