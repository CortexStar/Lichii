// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#include "video_core/photo_mode_state.h"

namespace VideoCore::PhotoMode {
namespace {
std::atomic<bool> hide_hud{};
std::atomic<u64> output_image{};
std::atomic<bool> hide_haze{};
std::atomic<u64> present_crop{};
std::atomic<u64> presented_frame_size{};
} // Anonymous namespace

void SetHideHud(bool hide) {
    hide_hud.store(hide, std::memory_order_relaxed);
}

bool HideHud() {
    return hide_hud.load(std::memory_order_relaxed);
}

void SetOutputImage(u64 gpu_address) {
    output_image.store(gpu_address, std::memory_order_relaxed);
}

u64 OutputImage() {
    return output_image.load(std::memory_order_relaxed);
}

void SetHideHaze(bool hide) {
    hide_haze.store(hide, std::memory_order_relaxed);
}

bool HideHaze() {
    return hide_haze.load(std::memory_order_relaxed);
}

void SetPresentCrop(u32 ratio_width, u32 ratio_height) {
    present_crop.store(ratio_width == 0 || ratio_height == 0
                           ? 0
                           : u64{ratio_width} | (u64{ratio_height} << 32),
                       std::memory_order_relaxed);
}

u64 PresentCrop() {
    return present_crop.load(std::memory_order_relaxed);
}

void SetPresentedFrameSize(u32 width, u32 height) {
    presented_frame_size.store(u64{width} | (u64{height} << 32), std::memory_order_relaxed);
}

u64 PresentedFrameSize() {
    return presented_frame_size.load(std::memory_order_relaxed);
}

} // namespace VideoCore::PhotoMode
