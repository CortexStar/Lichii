// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Tegra::Control {
struct ChannelState;
}

namespace VideoCommon::GpuRecord {

/// A channel's state at a command list boundary: every engine's registers and the state the
/// methods derived from them (constant buffer bindings, root tables, shadow RAM, MME data RAM,
/// the compute engine's native registers), the puller's registers and subchannel bindings.
/// Taken on the thread that owns the engines (the split GPU thread's back) with nothing queued.
[[nodiscard]] std::vector<u8> CaptureChannel(Tegra::Control::ChannelState& channel);

/// Writes a CaptureChannel blob into a channel that was initialized the same way (GPU
/// InitChannel), before its first command list, on the thread that owns the engines; marks
/// every register dirty. False (and `error`) when the blob doesn't fit this build's engines.
[[nodiscard]] bool RestoreChannel(Tegra::Control::ChannelState& channel, std::span<const u8> blob,
                                  std::string& error);

/// The Switch 2 macro code and bindings every channel shares (NativeMacroContext), and the
/// compute QMDs in flight (NativeQmdScheduler's table).
[[nodiscard]] std::vector<u8> CaptureSharedState(Tegra::Control::ChannelState& channel);
[[nodiscard]] bool RestoreSharedState(Tegra::Control::ChannelState& channel,
                                      std::span<const u8> blob, std::string& error);

} // namespace VideoCommon::GpuRecord
