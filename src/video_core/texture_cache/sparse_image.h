// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <new>
#include <vector>

#include "video_core/texture_cache/image_base.h"

namespace VideoCommon {

template <typename Bind, typename Upload>
bool PrepareSparseImageWrite(ImageFlagBits& flags, std::vector<LevelRect>& pending,
                             Bind&& bind, Upload&& upload) {
    if (False(flags & ImageFlagBits::SparseTiles)) {
        return false;
    }
    if (!bind(pending)) {
        throw std::bad_alloc{};
    }
    upload(pending);
    pending.clear();
    flags &= ~(ImageFlagBits::SparseTiles | ImageFlagBits::Unbacked);
    return true;
}

} // namespace VideoCommon
