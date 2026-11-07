// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel::Svc {

/// Ocarina of Time: svc 0xF3, placed by the built-in patch "Resource poll backoff" on the path where
/// the game's resource request loop (main+0xF7A0) would rescan its pending requests at once. Sleeps
/// the calling thread briefly and leaves every register as it was. Returns false (and does nothing)
/// for any other SVC.
bool CallNx2OotPollBackoff(Core::System& system, u32 imm);

} // namespace Kernel::Svc
