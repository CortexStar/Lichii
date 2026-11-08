// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The GPU recorder reads the engines' state when a recording starts and a replay writes it
// back (gpu_record/engine_state.cpp); the classes holding it name this struct as a friend.
namespace VideoCommon::GpuRecord {
struct StateAccess;
} // namespace VideoCommon::GpuRecord
