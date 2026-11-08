// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;

TEST_CASE("ensure fast dispatch entry is cleared even when a block does not have any patching requirements", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::FastDispatch));

    env.code_mem_start_address = 100;
    env.code_mem.clear();
    env.code_mem.emplace_back(0xd2800d80);  // MOV X0, 108
    env.code_mem.emplace_back(0xd61f0000);  // BR X0
    env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
    env.code_mem.emplace_back(0x14000000);  // B .

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(108, 4);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    env.code_mem[2] = 0xd28008a0;  // MOV X0, 69

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(108, 4);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);

    jit.SetPC(100);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);
}

TEST_CASE("ensure fast dispatch entry is cleared even when a block does not have any patching requirements 2", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::FastDispatch));

    env.code_mem.emplace_back(0xd2800100);  // MOV X0, 8
    env.code_mem.emplace_back(0xd61f0000);  // BR X0
    env.code_mem.emplace_back(0xd2800540);  // MOV X0, 42
    env.code_mem.emplace_back(0x14000000);  // B .

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(8, 4);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    env.code_mem[2] = 0xd28008a0;  // MOV X0, 69

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 42);

    jit.InvalidateCacheRange(8, 4);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);

    jit.SetPC(0);
    env.ticks_left = 4;
    CheckedRun([&]() { jit.Run(); });
    REQUIRE(jit.GetRegister(0) == 69);
}

TEST_CASE("invalidating a range recompiles only the blocks overlapping it, including blocks spanning two pages", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    // Block A runs from 0xff8 over the page boundary at 0x1000; block B starts at 0x2000.
    env.code_mem_start_address = 0xff8;
    env.code_mem.assign((0x2008 - 0xff8) / 4, 0x14000000);  // B .
    const auto at = [&](u64 vaddr) -> u32& { return env.code_mem[(vaddr - env.code_mem_start_address) / 4]; };
    at(0xff8) = 0xd28000a0;   // MOV X0, #5
    at(0xffc) = 0xd2800121;   // MOV X1, #9
    at(0x1000) = 0xd28000e2;  // MOV X2, #7
    at(0x2000) = 0xd2800103;  // MOV X3, #8

    const auto run = [&](u64 pc, u64 ticks) {
        jit.SetPC(pc);
        env.ticks_left = ticks;
        CheckedRun([&]() { jit.Run(); });
    };
    run(0xff8, 4);
    run(0x2000, 2);
    REQUIRE(jit.GetRegister(0) == 5);
    REQUIRE(jit.GetRegister(2) == 7);
    REQUIRE(jit.GetRegister(3) == 8);

    at(0x1000) = 0xd2800142;  // MOV X2, #10
    at(0x2000) = 0xd2800123;  // MOV X3, #9

    // Not invalidated yet: both blocks still run their old code.
    run(0xff8, 4);
    run(0x2000, 2);
    REQUIRE(jit.GetRegister(2) == 7);
    REQUIRE(jit.GetRegister(3) == 8);

    // A range ending right before block B, on block A's second page: A is recompiled, B is kept.
    jit.InvalidateCacheRange(0x1000, 0x1000);
    run(0xff8, 4);
    run(0x2000, 2);
    REQUIRE(jit.GetRegister(2) == 10);
    REQUIRE(jit.GetRegister(3) == 8);

    // Block A from its first page.
    at(0xff8) = 0xd28000c0;  // MOV X0, #6
    jit.InvalidateCacheRange(0xffc, 1);
    run(0xff8, 4);
    REQUIRE(jit.GetRegister(0) == 6);
    REQUIRE(jit.GetRegister(2) == 10);

    // Block B's first byte.
    jit.InvalidateCacheRange(0x2000, 1);
    run(0x2000, 2);
    REQUIRE(jit.GetRegister(3) == 9);
}
