// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/x64/atomic_fallback.h"
#include "dynarmic/interface/halt_reason.h"

namespace {

using Dynarmic::Backend::X64::AtomicCasFallback;
using Dynarmic::Backend::X64::AtomicFallbackContext;
using Dynarmic::Backend::X64::AtomicRmwFallback;

// One 32-bit word of guest memory as the memory callbacks see it, with another core's work
// scripted into the windows of the callback path.
struct Word final : Dynarmic::A64::UserCallbacks {
    u32 value{};
    /// Another core writes this into the word inside our next exclusive write, before its compare.
    std::optional<u32> write_inside_next_write;
    /// ... and this before our read after that (it unlocks again: an A-B-A).
    std::optional<u32> write_before_next_read;
    /// Refuse every exclusive write (an aborted access) and halt with this.
    u32 refuse_and_halt{};
    volatile u32* halt_reason{};
    u32 writes{};
    u32 refused{};

    std::uint64_t MemoryRead(Dynarmic::A64::VAddr, std::size_t) override {
        if (write_before_next_read && writes > 0) {
            value = *write_before_next_read;
            write_before_next_read.reset();
        }
        return value;
    }
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr) override {
        return {};
    }
    void MemoryWrite(Dynarmic::A64::VAddr, std::uint64_t data, std::size_t) override {
        value = static_cast<u32>(data);
    }
    void MemoryWrite128(Dynarmic::A64::VAddr, Dynarmic::A64::Vector) override {}
    bool MemoryWriteExclusive32(Dynarmic::A64::VAddr, std::uint32_t data,
                                std::uint32_t expected) override {
        ++writes;
        if (refuse_and_halt != 0) {
            *halt_reason |= refuse_and_halt;
            ++refused;
            return false;
        }
        if (write_inside_next_write) {
            value = *write_inside_next_write;
            write_inside_next_write.reset();
        }
        if (value != expected) {
            ++refused;
            return false;
        }
        value = data;
        return true;
    }
    void CallSVC(std::uint32_t) override {}
    void ExceptionRaised(Dynarmic::A64::VAddr, Dynarmic::A64::Exception) override {}
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override {
        return 0;
    }
    std::uint64_t GetCNTPCT() override {
        return 0;
    }
};

struct Fallback {
    Word word;
    Dynarmic::A64::UserConfig conf{};
    volatile u32 halt_reason = 0;
    AtomicFallbackContext context{&conf, &halt_reason};

    Fallback() {
        conf.callbacks = &word;
        word.halt_reason = &halt_reason;
    }
};

constexpr u32 OwnTag = 0x1234;
constexpr u32 OtherTag = 0x5678;

} // namespace

TEST_CASE("Atomic CAS fallback doesn't take a lock it never wrote", "[dynarmic][atomics]") {
    // nn::os's mutex lock is CASA 0 -> own handle. Another core locks the word while our
    // exclusive write compares and unlocks it before our next read. A CAS that then reported
    // success (0 read again) without writing left the caller owning a free lock: nn::os aborted at
    // the unlock, finding another thread's handle in the word.
    Fallback f;
    f.word.write_inside_next_write = OtherTag;
    f.word.write_before_next_read = 0;
    const u64 old = AtomicCasFallback<32>(f.context, 0x1000, 0, OwnTag);
    REQUIRE(f.word.refused == 1);
    REQUIRE(old == 0);
    REQUIRE(f.word.value == OwnTag);
}

TEST_CASE("Atomic CAS fallback reports the word another core holds", "[dynarmic][atomics]") {
    Fallback f;
    f.word.write_inside_next_write = OtherTag;
    const u64 old = AtomicCasFallback<32>(f.context, 0x1000, 0, OwnTag);
    REQUIRE(old == OtherTag);
    REQUIRE(f.word.value == OtherTag);
}

TEST_CASE("Atomic add fallback doesn't lose an addition", "[dynarmic][atomics]") {
    // LDADD of a reference count, with another core's +1 / -1 around our refused write.
    Fallback f;
    f.word.value = 5;
    f.word.write_inside_next_write = 6;
    f.word.write_before_next_read = 5;
    const u64 old = AtomicRmwFallback<32>(f.context, 0x1000, 1, 0);
    REQUIRE(old == 5);
    REQUIRE(f.word.value == 6);
}

TEST_CASE("Atomic fallbacks stop retrying at a memory abort", "[dynarmic][atomics]") {
    Fallback f;
    f.word.refuse_and_halt = static_cast<u32>(Dynarmic::HaltReason::MemoryAbort);
    AtomicCasFallback<32>(f.context, 0x1000, 0, OwnTag);
    REQUIRE(f.word.writes == 1);
    AtomicRmwFallback<32>(f.context, 0x1000, 1, 0);
    REQUIRE(f.word.writes == 2);
}

TEST_CASE("Atomic fallbacks keep retrying through other halts", "[dynarmic][atomics]") {
    // A halt for another reason (an interrupt) doesn't make a refused write count as done.
    Fallback f;
    f.halt_reason = static_cast<u32>(Dynarmic::HaltReason::UserDefined1);
    f.word.write_inside_next_write = OtherTag;
    f.word.write_before_next_read = 0;
    REQUIRE(AtomicCasFallback<32>(f.context, 0x1000, 0, OwnTag) == 0);
    REQUIRE(f.word.value == OwnTag);
}
