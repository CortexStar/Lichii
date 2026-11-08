// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// UserConfig::indirect_branch_cache: per-site caches of BR/BLR targets. Programs run with the
// cache on and off must end in the same state: monomorphic and megamorphic sites (past the miss
// cutoff), and a callee whose code changes between runs (the cache must not keep the old block).

#include <array>

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>

#include "./testenv.h"
#include "dynarmic/interface/A64/a64.h"

using namespace Dynarmic;
using namespace oaknut::util;

namespace {

class HaltOnSvcEnv final : public A64TestEnv {
public:
    A64::Jit* jit = nullptr;
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
};

A64::UserConfig MakeConfig(HaltOnSvcEnv& env, bool cache) {
    A64::UserConfig config{};
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    config.omit_block_frame = true;
    config.halt_check_on_back_edges = true;
    config.host_call_return = true;
    config.indirect_branch_cache = cache;
    return config;
}

std::array<u64, 31> Registers(const A64::Jit& jit) {
    std::array<u64, 31> r{};
    for (size_t i = 0; i < r.size(); ++i) {
        r[i] = jit.GetRegister(i);
    }
    return r;
}

template<typename Gen>
std::array<u64, 31> RunProgram(bool cache, Gen&& gen) {
    HaltOnSvcEnv env;
    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    gen(code);
    A64::Jit jit{MakeConfig(env, cache)};
    env.jit = &jit;
    for (int i = 0; i < 2; ++i) {
        jit.SetPC(0);
        jit.SetSP(0x80000);
        jit.Run();
    }
    return Registers(jit);
}

template<typename Gen>
std::array<u64, 31> RunBoth(Gen&& gen) {
    const auto off = RunProgram(false, gen);
    const auto on = RunProgram(true, gen);
    REQUIRE(on == off);
    return on;
}

}  // namespace

TEST_CASE("A64: indirect_branch_cache monomorphic and megamorphic sites", "[a64][indirect_branch_cache]") {
    const auto r = RunBoth([](auto& code) {
        oaknut::Label loop, a, cases, join;
        code.MOV(X0, XZR);
        code.MOV(X1, XZR);
        code.MOVZ(X19, 400);
        code.ADR(X9, a);
        code.ADR(X20, cases);
        code.l(loop);
        code.BLR(X9);  // monomorphic: always a
        // Megamorphic: BR to case counter & 3, cases 16 bytes apart.
        code.AND(X2, X19, 3);
        code.ADD(X3, X20, X2, LSL, 4);
        code.BR(X3);
        code.l(cases);
        for (int i = 0; i < 4; ++i) {
            code.ADD(X1, X1, 1 << (3 * i));
            code.B(join);
            code.NOP();
            code.NOP();
        }
        code.l(join);
        code.SUBS(X19, X19, 1);
        code.B(NE, loop);
        code.SVC(0);
        code.l(a);
        code.ADD(X0, X0, 3);
        code.RET();
    });
    REQUIRE(r[0] == 1200);
    REQUIRE(r[1] == 100 * (1 + 8 + 64 + 512));
}

TEST_CASE("A64: indirect_branch_cache after the callee's code changes", "[a64][indirect_branch_cache]") {
    for (const bool cache : {false, true}) {
        HaltOnSvcEnv env;
        oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
        oaknut::Label loop, f;
        code.MOV(X0, XZR);
        code.MOVZ(X19, 50);
        code.ADR(X9, f);
        code.l(loop);
        code.BLR(X9);
        code.SUBS(X19, X19, 1);
        code.B(NE, loop);
        code.SVC(0);
        code.l(f);
        const size_t patch_index = env.code_mem.size();
        code.ADD(X0, X0, 1);
        code.RET();

        A64::Jit jit{MakeConfig(env, cache)};
        env.jit = &jit;
        jit.SetPC(0);
        jit.SetSP(0x80000);
        jit.Run();
        REQUIRE(jit.GetRegister(0) == 50);

        // The callee now adds 2; the site's cached target must not run the old block.
        env.code_mem[patch_index] = 0x91000800;  // add x0, x0, #2
        jit.InvalidateCacheRange(patch_index * 4, 4);
        jit.SetPC(0);
        jit.Run();
        REQUIRE(jit.GetRegister(0) == 100);
    }
}
