// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/guest_hooks.h"

namespace {

using namespace Core::GuestHooks;

constexpr u64 Program = 0x0400C3F00006E000ULL;
constexpr u64 Base = 0x80878000;

const auto* const ProcessA = reinterpret_cast<const Kernel::KProcess*>(0x1000);
const auto* const ProcessB = reinterpret_cast<const Kernel::KProcess*>(0x2000);

std::vector<u8> Image(std::initializer_list<std::pair<u64, u32>> words, size_t size = 0x100) {
    std::vector<u8> image(size);
    for (const auto& [offset, word] : words) {
        std::memcpy(image.data() + offset, &word, sizeof(word));
    }
    return image;
}

Hook ReplaceHook(u64 offset = 0x40) {
    return Hook{
        .name = "test-replace",
        .program_id = Program,
        .module = "main",
        .offset = offset,
        .original = {0x11111111, 0x22222222},
        .replacement = {0xAAAAAAAA, 0xBBBBBBBB},
    };
}

struct FakeRegisters final : Registers {
    u64 x[31]{};
    u64 sp{};
    u64 pc{};
    u64 Get(u32 index) const override { return x[index]; }
    void Set(u32 index, u64 value) override { x[index] = value; }
    u64 GetSP() const override { return sp; }
    void SetSP(u64 value) override { sp = value; }
    u64 GetPC() const override { return pc; }
    void SetPC(u64 value) override { pc = value; }
};

} // namespace

TEST_CASE("Replace hooks activate only when every original word matches", "[core][guest_hooks]") {
    ResetForTesting();
    REQUIRE(Register(ReplaceHook()));
    auto other = Image({{0x40, 0x11111111}, {0x44, 0x33333333}});
    REQUIRE(OnModuleLoaded(ProcessA, Program, "main", Base, other) == 0);
    REQUIRE_FALSE(InstructionFor(ProcessA, Base + 0x40));

    auto image = Image({{0x40, 0x11111111}, {0x44, 0x22222222}});
    REQUIRE(OnModuleLoaded(ProcessA, Program, "sdk", Base, image) == 0);
    REQUIRE(OnModuleLoaded(ProcessA, Program + 1, "main", Base, image) == 0);
    REQUIRE(OnModuleLoaded(ProcessA, Program, "main", Base, image) == 1);
    ResetForTesting();
}

TEST_CASE("Replace hooks serve their words only inside their range and process",
          "[core][guest_hooks]") {
    ResetForTesting();
    REQUIRE(Register(ReplaceHook()));
    auto image = Image({{0x40, 0x11111111}, {0x44, 0x22222222}});
    REQUIRE(OnModuleLoaded(ProcessA, Program, "main", Base, image) == 1);
    REQUIRE(InstructionFor(ProcessA, Base + 0x40) == 0xAAAAAAAAu);
    REQUIRE(InstructionFor(ProcessA, Base + 0x44) == 0xBBBBBBBBu);
    REQUIRE_FALSE(InstructionFor(ProcessA, Base + 0x3C));
    REQUIRE_FALSE(InstructionFor(ProcessA, Base + 0x48));
    REQUIRE_FALSE(InstructionFor(ProcessB, Base + 0x40));

    REQUIRE(OnModuleLoaded(ProcessA, Program, "main", Base, image) == 1);
    OnProcessExit(ProcessA);
    REQUIRE_FALSE(InstructionFor(ProcessA, Base + 0x40));
    ResetForTesting();
}

TEST_CASE("Malformed hooks are refused", "[core][guest_hooks]") {
    ResetForTesting();
    Hook uneven = ReplaceHook();
    uneven.replacement.pop_back();
    REQUIRE_FALSE(Register(uneven));
    Hook misaligned = ReplaceHook(0x42);
    REQUIRE_FALSE(Register(misaligned));
    Hook empty = ReplaceHook();
    empty.original.clear();
    empty.replacement.clear();
    REQUIRE_FALSE(Register(empty));
    ResetForTesting();
}

namespace {
u64 g_native_calls = 0;
void NativeHandlerForTest(NativeCall& call) {
    ++g_native_calls;
    REQUIRE(call.module_base == Base);
    REQUIRE(call.hook_address == Base + 0x80);
    call.registers.Set(0, call.registers.Get(0) + 1);
    call.registers.SetPC(call.registers.Get(30));
}
} // namespace

TEST_CASE("Native hooks fetch a breakpoint and run their handler", "[core][guest_hooks]") {
    ResetForTesting();
    g_native_calls = 0;
    REQUIRE(Register(Hook{
        .name = "test-native",
        .program_id = Program,
        .module = "main",
        .offset = 0x80,
        .original = {0xD503237F, 0xD10083FF},
        .native = NativeHandlerForTest,
    }));
    auto image = Image({{0x80, 0xD503237F}, {0x84, 0xD10083FF}});
    REQUIRE(OnModuleLoaded(ProcessA, Program, "main", Base, image) == 1);
    REQUIRE(InstructionFor(ProcessA, Base + 0x80) == NativeBreakpoint);
    REQUIRE_FALSE(InstructionFor(ProcessA, Base + 0x84));

    FakeRegisters registers;
    registers.x[0] = 41;
    registers.x[30] = 0x1234;
    registers.pc = Base + 0x84;
    auto* const memory = reinterpret_cast<Core::Memory::Memory*>(0x10);
    REQUIRE_FALSE(RunNative(ProcessA, Base + 0x84, registers, *memory));
    REQUIRE_FALSE(RunNative(ProcessB, Base + 0x80, registers, *memory));
    REQUIRE(RunNative(ProcessA, Base + 0x80, registers, *memory));
    REQUIRE(g_native_calls == 1);
    REQUIRE(registers.x[0] == 42);
    REQUIRE(registers.pc == 0x1234);
    ResetForTesting();
}
