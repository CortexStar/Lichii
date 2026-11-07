// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
};

} // namespace VideoCore
