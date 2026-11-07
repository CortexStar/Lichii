// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCore {

/// The cache that reported a flush area (GetFlushArea), for statistics (GpuWaitStats).
enum class DownloadAreaSource : u8 {
    None,
    TextureCache,
    BufferCache,
};

struct RasterizerDownloadArea {
    VAddr start_address;
    VAddr end_address;
    bool preemtive;
    DownloadAreaSource source{};
    /// What guest memory lacks there is on its way for an earlier read: nothing to flush, only to
    /// wait for (RasterizerInterface::FinishRead).
    bool in_flight{};
};

/// Downloads for guest CPU reads that are submitted but not yet written to guest memory
/// (RasterizerInterface::FlushRegionForRead). While there are any, a write to guest memory from
/// the GPU's side first finishes those it overlaps (FinishRead), so none lands over it later.
inline std::atomic<u32> g_read_downloads{};

} // namespace VideoCore
