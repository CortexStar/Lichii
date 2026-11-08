// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <thread>
#include <catch2/catch_test_macros.hpp>
#include "common/nx2_address_wait.h"

using namespace std::chrono_literals;
using Common::Nx2AddressWait::WaitSpinning;
using Common::Nx2AddressWait::WakeOne;

TEST_CASE("WaitSpinning returns at once when the word already changed", "[nx2_address_wait]") {
    std::atomic<u32> word{1};
    WaitSpinning(word, 0, 100us);
    WaitSpinning(word, 0, 0us);
    CHECK(word.load() == 1);
}

TEST_CASE("WaitSpinning wakes on every store, within and after the spin", "[nx2_address_wait]") {
    // A waker that stores during the PAUSE phase, during the MWAITX phase and after the spin
    // (the OS wait) must always be seen; a lost wake-up hangs the test.
    for (const auto delay : {0us, 1us, 10us, 60us, 2000us}) {
        for (int round = 0; round < 50; ++round) {
            std::atomic<u32> word{0};
            std::thread waker{[&] {
                const auto until = std::chrono::steady_clock::now() + delay;
                while (std::chrono::steady_clock::now() < until) {
                }
                word.store(1, std::memory_order_release);
                WakeOne(word);
            }};
            WaitSpinning(word, 0, 20us);
            CHECK(word.load(std::memory_order_acquire) == 1);
            waker.join();
        }
    }
}

TEST_CASE("WaitSpinning ping-pong keeps every handoff", "[nx2_address_wait]") {
    // Two threads hand a token back and forth, as an HLE request and its reply do.
    std::atomic<u32> to_server{0};
    std::atomic<u32> to_client{0};
    constexpr u32 Rounds = 20'000;
    std::thread server{[&] {
        for (u32 i = 1; i <= Rounds; ++i) {
            WaitSpinning(to_server, i - 1, 20us);
            to_client.store(i, std::memory_order_release);
            WakeOne(to_client);
        }
    }};
    for (u32 i = 1; i <= Rounds; ++i) {
        to_server.store(i, std::memory_order_release);
        WakeOne(to_server);
        WaitSpinning(to_client, i - 1, 20us);
    }
    server.join();
    CHECK(to_client.load() == Rounds);
}
