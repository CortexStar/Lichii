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

TEST_CASE("return stack buffer survives a code cache clear inside a call", "[a64]") {
    A64TestEnv env;
    A64::UserConfig conf{};
    conf.callbacks = &env;
    // Small enough that each pass through the callee emits enough code to clear the cache.
    conf.code_cache_size = 2 * 1024 * 1024;
    A64::Jit jit{conf};

    REQUIRE(conf.HasOptimization(OptimizationFlag::ReturnStackBuffer));

    constexpr u64 blocks = 60000;
    env.code_mem.clear();
    env.code_mem.emplace_back(0x94000005);  // BL callee
    env.code_mem.emplace_back(0x91000400);  // ADD X0, X0, #1
    env.code_mem.emplace_back(0xF100081F);  // CMP X0, #2
    env.code_mem.emplace_back(0x54FFFFA1);  // B.NE 0
    env.code_mem.emplace_back(0x14000000);  // B .
    for (u64 i = 0; i < blocks; ++i) {      // callee:
        env.code_mem.emplace_back(0x91000421);  // ADD X1, X1, #1
        env.code_mem.emplace_back(0x14000001);  // B .+4
    }
    env.code_mem.emplace_back(0xD65F03C0);  // RET

    jit.SetPC(0);
    env.ticks_left = 2 * (2 * blocks + 5) + 16;
    CheckedRun([&]() { jit.Run(); });

    REQUIRE(jit.GetRegister(0) == 2);
    REQUIRE(jit.GetRegister(1) == 2 * blocks);
    REQUIRE(jit.GetPC() == 16);
}
