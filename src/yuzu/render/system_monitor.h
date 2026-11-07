// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <thread>

#include "common/common_types.h"

// Samples host CPU, GPU and memory usage on a background thread.
class SystemMonitor {
public:
    struct Usage {
        u64 used = 0;
        u64 total = 0;

        double Fraction() const {
            return total ? double(used) / double(total) : 0.0;
        }
    };

    struct Snapshot {
        std::optional<double> cpu_percent;
        std::optional<double> gpu_percent;
        std::optional<Usage> ram;
        std::optional<Usage> vram;
    };

    SystemMonitor();
    ~SystemMonitor();

    // Sampling only runs between Start() and Stop()
    void Start();
    void Stop();

    Snapshot Get() const;

private:
    void Run(std::stop_token token);

    mutable std::mutex mutex;
    Snapshot snapshot;
    std::jthread thread;
};
