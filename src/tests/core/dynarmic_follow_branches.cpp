// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <optional>
#include <catch2/catch_test_macros.hpp>
#include "common/common_types.h"
#include "dynarmic/interface/A64/a64.h"

namespace {

class Env final : public Dynarmic::A64::UserCallbacks {
public:
    Dynarmic::A64::Jit* jit{};
    std::map<u64, u32> code;
    u64 svcs{};

    std::optional<u32> MemoryReadCode(u64 address) override {
        const auto it = code.find(address);
        return it == code.end() ? std::nullopt : std::optional<u32>{it->second};
    }
    u64 MemoryRead(u64, size_t) override {
        FAIL("Unexpected memory read");
        return 0;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        FAIL("Unexpected memory read");
        return {};
    }
    void MemoryWrite(u64, u64, size_t) override {
        FAIL("Unexpected memory write");
    }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {
        FAIL("Unexpected memory write");
    }
    void CallSVC(u32) override {
        ++svcs;
        jit->HaltExecution();
    }
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) override {
        FAIL("Unexpected exception " << static_cast<int>(exception) << " at " << pc);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1000; }
    u64 GetCNTPCT() override { return 0; }
};

std::map<u64, u32> Program() {
    return {
        {0x1000, 0xD2800020},
        {0x1004, 0x9400003F},
        {0x1008, 0x91000C00},
        {0x100C, 0x1400007D},
        {0x1100, 0xD37EF400},
        {0x1104, 0x91001400},
        {0x1108, 0xD65F03C0},
        {0x1200, 0x91000442},
        {0x1204, 0xF100285F},
        {0x1208, 0x54000060},
        {0x120C, 0x17FFFFFD},
        {0x1214, 0xD4000001},
    };
}

struct Result {
    u64 x0, x2, x30, pc;
};

Result Run(Env& env, Dynarmic::A64::Jit& jit) {
    jit.SetPC(0x1000);
    jit.SetSP(0x80000);
    for (size_t i = 0; i < 31; ++i) {
        jit.SetRegister(i, 0);
    }
    env.svcs = 0;
    jit.Run();
    REQUIRE(env.svcs == 1);
    return {jit.GetRegister(0), jit.GetRegister(2), jit.GetRegister(30), jit.GetPC()};
}

Dynarmic::A64::UserConfig Config(Env& env, bool follow) {
    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    config.follow_branches = follow;
    return config;
}

} // namespace

TEST_CASE("Following B and BL gives the same results as ending blocks there",
          "[dynarmic][follow_branches]") {
    Result results[2];
    for (const bool follow : {false, true}) {
        Env env;
        env.code = Program();
        Dynarmic::A64::Jit jit{Config(env, follow)};
        env.jit = &jit;
        results[follow] = Run(env, jit);
        const Result again = Run(env, jit);
        REQUIRE(again.x0 == results[follow].x0);
    }
    for (const auto& r : results) {
        REQUIRE(r.x0 == 12);
        REQUIRE(r.x2 == 10);
        REQUIRE(r.x30 == 0x1008);
        REQUIRE(r.pc == 0x1218);
    }
}

TEST_CASE("Changing a followed callee invalidates the block that inlined it",
          "[dynarmic][follow_branches]") {
    Env env;
    env.code = Program();
    Dynarmic::A64::Jit jit{Config(env, true)};
    env.jit = &jit;
    REQUIRE(Run(env, jit).x0 == 12);
    env.code[0x1104] = 0x91001800;
    jit.InvalidateCacheRange(0x1104, 4);
    REQUIRE(Run(env, jit).x0 == 13);
    env.code[0x1008] = 0x91001000;
    jit.InvalidateCacheRange(0x1008, 4);
    REQUIRE(Run(env, jit).x0 == 14);
}

TEST_CASE("A block that follows no branch keeps its own range after one that did",
          "[dynarmic][follow_branches]") {
    Env env;
    env.code = Program();
    Dynarmic::A64::Jit jit{Config(env, true)};
    env.jit = &jit;
    REQUIRE(Run(env, jit).x2 == 10);
    env.code[0x1214] = 0x91019042;
    env.code[0x1218] = 0xD4000001;
    jit.InvalidateCacheRange(0x1214, 8);
    const Result r = Run(env, jit);
    REQUIRE(r.x2 == 110);
    REQUIRE(r.pc == 0x121C);
}
