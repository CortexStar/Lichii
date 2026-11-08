// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <map>
#include <optional>
#include <span>
#include <catch2/catch_test_macros.hpp>
#include "common/common_types.h"
#include "dynarmic/interface/A64/a64.h"

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace {

constexpr u64 Entry = 0x10000;
constexpr u32 Svc0 = 0xD4000001;

/// Guest code at Entry, flat little-endian data memory, SVC #0 stops the run.
class GuestEnvironment : public Dynarmic::A64::UserCallbacks {
public:
    Dynarmic::A64::Jit* jit{};
    std::map<u64, u32> code;
    std::map<u64, u8> data;
    u64 callback_counter_reads{};
    u64 host_function_pc{~u64{0}};
    std::optional<Dynarmic::A64::HostFunctionEntry> host_function;
    u32 host_function_queries{};

    std::optional<u32> MemoryReadCode(u64 address) override {
        const auto it = code.find(address);
        if (it == code.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    u64 MemoryRead(u64 address, size_t size) override {
        u64 value = 0;
        for (size_t i = 0; i < size; ++i) {
            value |= u64(data[address + i]) << (8 * i);
        }
        return value;
    }
    Dynarmic::A64::Vector MemoryRead128(u64 address) override {
        return {MemoryRead(address, 8), MemoryRead(address + 8, 8)};
    }
    void MemoryWrite(u64 address, u64 value, size_t size) override {
        for (size_t i = 0; i < size; ++i) {
            data[address + i] = u8(value >> (8 * i));
        }
    }
    void MemoryWrite128(u64 address, Dynarmic::A64::Vector value) override {
        MemoryWrite(address, value[0], 8);
        MemoryWrite(address + 8, value[1], 8);
    }
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
    void ExceptionRaised(u64, Dynarmic::A64::Exception) override {}
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        ++callback_counter_reads;
        return 0;
    }

    std::optional<Dynarmic::A64::HostFunctionEntry> GetHostFunction(u64 address) override {
        if (address != host_function_pc) {
            return std::nullopt;
        }
        ++host_function_queries;
        return host_function;
    }

    void Load(std::initializer_list<u32> instructions, u64 address = Entry) {
        for (const u32 instruction : instructions) {
            code[address] = instruction;
            address += 4;
        }
    }
    void Load(std::span<const u32> instructions, u64 address) {
        for (const u32 instruction : instructions) {
            code[address] = instruction;
            address += 4;
        }
    }
};

Dynarmic::A64::UserConfig BaseConfig(GuestEnvironment& env) {
    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    config.wall_clock_cntpct = true;
    return config;
}

} // namespace

#if defined(__x86_64__) || defined(_M_X64)
TEST_CASE("Inline CNTPCT_EL0 reads scale the host TSC", "[guest_cpu]") {
    GuestEnvironment env;
    // mrs x0, cntpct_el0; mrs x1, cntpct_el0; isb; mrs x2, cntpct_el0; svc #0
    env.Load({0xD53BE020, 0xD53BE021, 0xD5033FDF});
    env.code[Entry + 12] = 0xD53BE022;
    env.code[Entry + 16] = Svc0;
    // Factor 2^63: the counter is TSC / 2.
    const u64 factor = u64{1} << 63;
    u64 last_tsc = 0;
    auto config = BaseConfig(env);
    config.cntpct_rdtsc_factor = &factor;
    config.cntpct_last_tsc = &last_tsc;
    config.cntpct_spin_tsc = 1000;
    Dynarmic::A64::Jit jit{config};
    env.jit = &jit;

    const u64 before = __rdtsc();
    jit.SetPC(Entry);
    jit.Run();
    const u64 after = __rdtsc();

    CHECK(env.callback_counter_reads == 0);
    const u64 x0 = jit.GetRegister(0);
    const u64 x1 = jit.GetRegister(1);
    const u64 x2 = jit.GetRegister(2);
    CHECK(x0 >= before / 2);
    CHECK(x0 <= x1);
    CHECK(x1 <= x2);
    CHECK(x2 <= after / 2);
    // The last read's TSC is kept for the polling check.
    CHECK(last_tsc >= before);
    CHECK(last_tsc <= after);
    CHECK(last_tsc / 2 == x2);
}

TEST_CASE("CNTPCT_EL0 without a factor uses the callback", "[guest_cpu]") {
    GuestEnvironment env;
    env.Load({0xD53BE020, Svc0});
    Dynarmic::A64::Jit jit{BaseConfig(env)};
    env.jit = &jit;
    jit.SetPC(Entry);
    jit.Run();
    CHECK(env.callback_counter_reads == 1);
    CHECK(jit.GetRegister(0) == 0);
}
#endif

namespace {

constexpr u64 Function = Entry + 0x1000;
constexpr u64 Stop = Entry + 0x2000;

struct XorShift {
    u64 state;
    u64 Next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

} // namespace

namespace {

struct HostCalls {
    bool handle{};
    u32 calls{};
};

bool AnswerFortyTwo(void* user, const Dynarmic::A64::HostFunctionState& state) {
    auto& host = *static_cast<HostCalls*>(user);
    ++host.calls;
    if (!host.handle) {
        return false;
    }
    state.x[0] = 42;
    return true;
}

} // namespace

TEST_CASE("A host function replaces a guest function and its guest code runs if it declines",
          "[guest_cpu]") {
    GuestEnvironment env;
    // bl Function; svc #0. Function: movz x0, #1; ret
    env.Load({0x94000400, Svc0});
    env.Load({0xD2800020, 0xD65F03C0}, Function);
    HostCalls host;
    env.host_function_pc = Function;
    env.host_function = Dynarmic::A64::HostFunctionEntry{&AnswerFortyTwo, &host};
    Dynarmic::A64::Jit jit{BaseConfig(env)};
    env.jit = &jit;

    const auto call = [&](bool handle) {
        host.handle = handle;
        jit.SetRegister(0, 0);
        jit.SetPC(Entry);
        jit.Run();
        return jit.GetRegister(0);
    };
    CHECK(call(true) == 42);
    CHECK(jit.GetPC() == Entry + 8);
    CHECK(call(false) == 1);
    CHECK(jit.GetPC() == Entry + 8);
    CHECK(call(false) == 1);
    CHECK(call(true) == 42);
    CHECK(host.calls == 4);
    // Translated once, the fallback block included.
    CHECK(env.host_function_queries == 1);

    // Invalidating the function's code drops both blocks.
    jit.InvalidateCacheRange(Function, 4);
    CHECK(call(false) == 1);
    CHECK(env.host_function_queries == 2);
    CHECK(host.calls == 5);
}
namespace {
u32 Crc32IsoReference(u32 crc, u64 value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        crc ^= static_cast<u8>(value >> (8 * i));
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xEDB88320U : 0U);
        }
    }
    return crc;
}
} // namespace

TEST_CASE("CRC32B and CRC32H are exact for every input byte and halfword", "[guest_cpu]") {
    GuestEnvironment env;
    // crc32b w0, w0, w1; svc #0; crc32h w0, w0, w1; svc #0
    env.Load({0x1AC14000, Svc0, 0x1AC14400, Svc0});
    Dynarmic::A64::Jit jit{BaseConfig(env)};
    env.jit = &jit;
    XorShift rng{777};
    const auto run = [&](u64 pc, u32 crc, u64 data) {
        jit.SetRegister(0, crc);
        // Bits above the operand are ignored.
        jit.SetRegister(1, data);
        jit.SetPC(pc);
        jit.Run();
        return jit.GetRegister(0);
    };
    u32 mismatches = 0;
    for (u32 byte = 0; byte < 256; ++byte) {
        for (int trial = 0; trial < 512; ++trial) {
            const u32 crc = trial == 0 ? 0 : trial == 1 ? ~0U : static_cast<u32>(rng.Next());
            const u64 data = (rng.Next() << 8) | byte;
            mismatches += run(Entry, crc, data) != Crc32IsoReference(crc, byte, 1);
        }
    }
    for (u32 half = 0; half < 0x10000; ++half) {
        for (int trial = 0; trial < 8; ++trial) {
            const u32 crc = trial == 0 ? 0 : trial == 1 ? ~0U : static_cast<u32>(rng.Next());
            const u64 data = (rng.Next() << 16) | half;
            mismatches += run(Entry + 8, crc, data) != Crc32IsoReference(crc, half, 2);
        }
    }
    CHECK(mismatches == 0);
}
