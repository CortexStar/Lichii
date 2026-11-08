// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/common_types.h"
#include "core/arm/guest_functions.h"
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
namespace {

// MurmurHash3_x86_32 of a NUL-terminated string, built with clang -O2 for aarch64 from the
// public-domain reference (strlen, 4-byte blocks, tail, finalizer).
constexpr std::array<u32, 67> MurmurCString{
    0x12800008, 0x11000508, 0x38684809, 0x35FFFFC9, 0x5285AA2A, 0x5286B269, 0x7100111F, 0x72B993CA,
    0x72A370E9, 0x54000062, 0x2A1F03EB, 0x1400001A, 0x528D6C8C, 0x2A1F03ED, 0x72BCCA8C, 0x110005AB,
    0x386D480E, 0x110009AF, 0x386B480B, 0x2A0B21CB, 0x386F480E, 0x11000DAF, 0x2A0E416B, 0x386F480E,
    0x110021AF, 0x6B0801FF, 0x2A0E616B, 0x1B0A7D6B, 0x138B456B, 0x1B097D6B, 0x4A01016B, 0x138B4D6B,
    0x0B0B096E, 0x110011AB, 0x2A0B03ED, 0x0B0C01C1, 0x54FFFD69, 0x1200050D, 0x2A1F03EC, 0x710005BF,
    0x5400006C, 0x3500012D, 0x1400000E, 0x710009BF, 0x8B2B400D, 0x54000060, 0x394009AC, 0x53103D8C,
    0x394005AD, 0x2A0D218C, 0x386B480B, 0x4A0B018B, 0x1B0A7D6A, 0x138A454A, 0x1B097D49, 0x4A010121,
    0x4A080028, 0x52994D69, 0x4A484108, 0x72B0BD69, 0x1B097D08, 0x5295C6A9, 0x72B85649, 0x4A483508,
    0x1B097D08, 0x4A484100, 0xD65F03C0,
};

} // namespace

TEST_CASE("MurmurHash3X86_32 matches the reference run in the JIT", "[guest_cpu]") {
    GuestEnvironment env;
    env.Load(MurmurCString, Function);
    env.Load({Svc0}, Stop);
    Dynarmic::A64::Jit jit{BaseConfig(env)};
    env.jit = &jit;
    constexpr u64 Text = 0x100000;
    XorShift rng{12345};
    u32 mismatches = 0;
    for (u32 length = 0; length < 200; ++length) {
        for (int trial = 0; trial < 20; ++trial) {
            std::string text;
            for (u32 i = 0; i < length; ++i) {
                text.push_back(static_cast<char>(1 + rng.Next() % 255));
            }
            for (u32 i = 0; i <= length; ++i) {
                env.data[Text + i] = i < length ? static_cast<u8>(text[i]) : 0;
            }
            const u32 seed = static_cast<u32>(rng.Next());
            jit.SetRegister(0, Text);
            jit.SetRegister(1, seed);
            jit.SetRegister(30, Stop);
            jit.SetPC(Function);
            jit.Run();
            Core::MurmurHash3X86_32 hash{seed};
            // In two pieces: the host function hashes page by page.
            const auto bytes = std::span{reinterpret_cast<const u8*>(text.data()), text.size()};
            hash.Update(bytes.first(length / 3));
            hash.Update(bytes.subspan(length / 3));
            mismatches += static_cast<u32>(jit.GetRegister(0)) != hash.Finish();
        }
    }
    CHECK(mismatches == 0);
}
namespace {

// musl's fmodf (src/math/fmodf.c, MIT) built with clang -O2 for aarch64.
constexpr std::array<u32, 93> MuslFmodf{
    0x1E26002C, 0x531F798A, 0x3400032A, 0x1E260028, 0x52AFF009, 0x12007908, 0x6B09011F, 0x54000288,
    0x1E260009, 0x53177928, 0x7103FD1F, 0x54000200, 0x531F792B, 0x6B0A017F, 0x54000209, 0x5317798B,
    0x34000248, 0x52A0100A, 0x3300592A, 0x3400032B, 0x52A0100D, 0x3300598D, 0x6B0B011F, 0x540004AC,
    0x6B0D014B, 0x54000565, 0x1400002C, 0x1E210800, 0x1E201800, 0xD65F03C0, 0x2F00E401, 0x1E210801,
    0x1E200C20, 0xD65F03C0, 0x5317592A, 0x2A1F03E8, 0x37F8008A, 0x531F794A, 0x51000508, 0x36FFFFCA,
    0x5280002A, 0x4B08014A, 0x1ACA212A, 0x35FFFD2B, 0x5317598D, 0x2A1F03EB, 0x37F8008D, 0x531F79AD,
    0x5100056B, 0x36FFFFCD, 0x5280002D, 0x4B0B01AD, 0x1ACD218D, 0x6B0B011F, 0x540000CC, 0x17FFFFE1,
    0x51000508, 0x531F794A, 0x6B0B011F, 0x540000CD, 0x6B0D014C, 0x54FFFF64, 0x2A0C03EA, 0x35FFFF2C,
    0x1400000F, 0x2A0B03E8, 0x6B0D014B, 0x54000064, 0x2A0B03EA, 0x3400014B, 0x53177D4B, 0x12010129,
    0x3500014B, 0x531F794B, 0x7150015F, 0x51000508, 0x2A0B03EA, 0x54FFFF83, 0x14000005, 0x2F00E401,
    0x1E210800, 0xD65F03C0, 0x2A0A03EB, 0x5280002A, 0x5160016C, 0x7100051F, 0x4B08014A, 0x1ACA256A,
    0x2A085D8B, 0x1A8BB148, 0x2A090108, 0x1E270100, 0xD65F03C0,
};

} // namespace

TEST_CASE("Fmodf matches musl fmodf run in the JIT", "[guest_cpu]") {
    GuestEnvironment env;
    env.Load(MuslFmodf, Function);
    env.Load({Svc0}, Stop);
    Dynarmic::A64::Jit jit{BaseConfig(env)};
    env.jit = &jit;

    std::vector<u32> values{
        0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x00000003, 0x0000FFFF, 0x00400000,
        0x007FFFFF, 0x807FFFFF, 0x00800000, 0x00800001, 0x01000000, 0x33800000, 0x3F000000,
        0x3F800000, 0xBF800000, 0x3F800001, 0x40000000, 0x40490FDB, 0xC0490FDB, 0x4B000000,
        0x4B7FFFFF, 0x5F000000, 0x7F000000, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000,
        0x7FC00000, 0xFFC00001, 0x7FFFFFFF, 0x7F800001, 0xFFA00000, 0x7FBFFFFF,
    };
    XorShift rng{0x9E3779B97F4A7C15};
    std::vector<std::pair<u32, u32>> pairs;
    for (const u32 x : values) {
        for (const u32 y : values) {
            pairs.emplace_back(x, y);
        }
    }
    for (int i = 0; i < 200'000; ++i) {
        const u64 r = rng.Next();
        pairs.emplace_back(static_cast<u32>(r), static_cast<u32>(r >> 32));
    }
    // Every exponent distance, with random mantissas: the shift-subtract loop of every length.
    for (u32 ex = 0; ex < 255; ++ex) {
        for (u32 ey = 0; ey <= ex; ++ey) {
            const u64 r = rng.Next();
            const u32 x = (ex << 23) | (static_cast<u32>(r) & 0x807FFFFF);
            const u32 y = (ey << 23) | (static_cast<u32>(r >> 32) & 0x807FFFFF);
            pairs.emplace_back(x, y);
        }
    }

    for (const u32 fpcr : {0U, 1U << 25, 1U << 24, 3U << 24}) {
        jit.SetFpcr(fpcr);
        // Dynarmic does not raise IDC when FZ flushes an input; the host function does, as the
        // Arm FPUnpack pseudocode says. Compare it only without FZ. musl's FP operations are all
        // exact (products with a zero, infinity or NaN, and quotients of a zero or infinity by
        // itself), so IXC is never raised; dynarmic's x64 FDIV reports a stray IXC for 0/0 and
        // the JIT run can carry one, so IXC is left out.
        const u32 flag_mask = (fpcr & (1U << 24)) != 0 ? 0x0F : 0x8F;
        u32 mismatches = 0;
        for (const auto [x, y] : pairs) {
            jit.SetVector(0, {x, 0x1111});
            jit.SetVector(1, {y, 0x2222});
            jit.SetFpsr(0);
            jit.SetRegister(30, Stop);
            jit.SetPC(Function);
            jit.Run();
            const auto expected = Core::Fmodf(x, y, fpcr);
            const auto v0 = jit.GetVector(0);
            const u32 fpsr = jit.GetFpsr() & 0x9F;
            if (v0[0] != expected.value || v0[1] != 0 ||
                (fpsr & flag_mask) != (expected.fpsr & flag_mask)) {
                if (++mismatches <= 8) {
                    UNSCOPED_INFO("fpcr " << std::hex << fpcr << " x " << x << " y " << y
                                          << ": guest " << v0[0] << " fpsr " << fpsr
                                          << ", host " << expected.value << " fpsr "
                                          << expected.fpsr);
                }
            }
        }
        CHECK(mismatches == 0);
    }
}
