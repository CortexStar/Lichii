// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <utility>

namespace Service::Nvidia::Devices {

class Nx2GpuMappingPreparation {
public:
    explicit Nx2GpuMappingPreparation(std::function<void()> rollback_ = {})
        : rollback{std::move(rollback_)} {}

    Nx2GpuMappingPreparation(Nx2GpuMappingPreparation&& other) noexcept
        : rollback{std::exchange(other.rollback, {})} {}

    ~Nx2GpuMappingPreparation() {
        if (rollback) {
            rollback();
        }
    }

    Nx2GpuMappingPreparation(const Nx2GpuMappingPreparation&) = delete;
    Nx2GpuMappingPreparation& operator=(const Nx2GpuMappingPreparation&) = delete;

    void Commit() {
        rollback = {};
    }

private:
    std::function<void()> rollback;
};

} // namespace Service::Nvidia::Devices
