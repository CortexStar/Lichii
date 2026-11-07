// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"

TEST_CASE("Nx2WaitAsyncReportsSettled returns at once without pending reports",
          "[video_core][nx2]") {
    constexpr u64 address = 0x10'0000;
    Tegra::Nx2IssueAsyncReport(address);
    Tegra::Nx2CompleteAsyncReport(address);
    Tegra::Nx2WaitAsyncReportsSettled(address, 0x20);
    REQUIRE(Tegra::Nx2AsyncReportsSettled(address, 0x20));
}

TEST_CASE("Nx2WaitAsyncReportsSettled waits for every report in the range", "[video_core][nx2]") {
    constexpr u64 address = 0x20'0000;
    Tegra::Nx2IssueAsyncReport(address);
    Tegra::Nx2IssueAsyncReport(address + 0x10);
    std::atomic<int> completed{};
    std::jthread completer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        completed.store(1);
        Tegra::Nx2CompleteAsyncReport(address);
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        completed.store(2);
        Tegra::Nx2CompleteAsyncReport(address + 0x10);
    });
    Tegra::Nx2WaitAsyncReportsSettled(address, 0x20);
    REQUIRE(completed.load() == 2);
}

TEST_CASE("Nx2WaitAsyncReportsSettled ignores reports outside the range", "[video_core][nx2]") {
    constexpr u64 address = 0x30'0000;
    Tegra::Nx2IssueAsyncReport(address + 0x100);
    Tegra::Nx2WaitAsyncReportsSettled(address, 0x10);
    REQUIRE_FALSE(Tegra::Nx2AsyncReportsSettled(address + 0x100, 0x10));
    Tegra::Nx2CompleteAsyncReport(address + 0x100);
}
