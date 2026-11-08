// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/query_cache/conditional_rendering.h"

namespace {

using VideoCommon::ConditionalRendering;
using VideoCommon::ConditionOperands;
using VideoCommon::ConditionPath;
using VideoCommon::DecideConditionPath;
using VideoCommon::HandlingOf;

// Guest memory at the two operands: zero or not. Records which operands were read.
struct Memory {
    std::array<bool, 2> zero{};
    std::vector<size_t> reads;
    bool operator()(size_t i) {
        reads.push_back(i);
        return zero[i];
    }
};

ConditionPath Decide(const ConditionOperands& operands, Memory& memory, bool extension = true,
                     bool driver_bypass = false, bool gpu_high = true) {
    return DecideConditionPath(extension, operands, driver_bypass, gpu_high, memory);
}

} // namespace

TEST_CASE("Conditions no cache holds are read from guest memory as it stands",
          "[video_core][conditional_rendering]") {
    Memory memory;
    const ConditionPath path = Decide({}, memory);
    REQUIRE(path == ConditionPath::Uncached);
    REQUIRE(memory.reads.empty());
    REQUIRE(HandlingOf(path, false) == ConditionalRendering::GuestMemory);
}

TEST_CASE("Conditions on queries synced to guest memory are read as it stands",
          "[video_core][conditional_rendering]") {
    Memory memory;
    const ConditionPath path = Decide({.in_query_cache = {true, true}}, memory);
    REQUIRE(path == ConditionPath::GuestSynchronized);
    REQUIRE(HandlingOf(path, false) == ConditionalRendering::GuestMemory);
}

TEST_CASE("Data a flush would write over the operands keeps the flushing read",
          "[video_core][conditional_rendering]") {
    REQUIRE(HandlingOf(ConditionPath::Uncached, true) == ConditionalRendering::Flush);
    REQUIRE(HandlingOf(ConditionPath::GuestSynchronized, true) == ConditionalRendering::Flush);
}

TEST_CASE("Without the extension the CPU decides after a flush",
          "[video_core][conditional_rendering]") {
    Memory memory;
    const ConditionPath path = Decide({.gpu_modified = {true, false}}, memory, false);
    REQUIRE(path == ConditionPath::ExtensionUnavailable);
    REQUIRE(memory.reads.empty());
    REQUIRE(HandlingOf(path, false) == ConditionalRendering::Flush);
}

TEST_CASE("A query not yet in guest memory is never read from it",
          "[video_core][conditional_rendering]") {
    Memory memory;
    memory.zero = {true, true};
    // Both operands are queries the cache holds, one not synced: no guest memory read decides.
    const ConditionPath path =
        Decide({.in_query_cache = {true, true}, .query_dirty = true}, memory);
    REQUIRE(path == ConditionPath::QueryCacheBypass);
    REQUIRE(memory.reads.empty());
    REQUIRE(HandlingOf(path, false) == ConditionalRendering::Host);
}

TEST_CASE("GPU-written operands are decided on the host", "[video_core][conditional_rendering]") {
    Memory memory;
    REQUIRE(Decide({.gpu_modified = {true, true}}, memory) == ConditionPath::ResolveCompare);
    REQUIRE(memory.reads.empty());
    REQUIRE(Decide({.gpu_modified = {true, true}}, memory, true, true) ==
            ConditionPath::DriverBypass);
    REQUIRE(Decide({.gpu_modified = {true, true}}, memory, true, false, false) ==
            ConditionPath::AccuracyBypass);
    for (const ConditionPath path : {ConditionPath::ResolveCompare, ConditionPath::DriverBypass,
                                     ConditionPath::AccuracyBypass, ConditionPath::InitialZero,
                                     ConditionPath::CurrentZero, ConditionPath::QueryCacheBypass}) {
        REQUIRE(HandlingOf(path, false) == ConditionalRendering::Host);
        REQUIRE(HandlingOf(path, true) == ConditionalRendering::Host);
    }
}

TEST_CASE("An uncached zero operand turns the pair into a one-operand compare",
          "[video_core][conditional_rendering]") {
    Memory memory;
    memory.zero = {false, true};
    // The first operand is GPU-written, the second is in no cache and reads zero.
    REQUIRE(Decide({.gpu_modified = {true, false}}, memory) == ConditionPath::CurrentZero);
    REQUIRE(memory.reads == std::vector<size_t>{1});
    Memory first_zero;
    first_zero.zero = {true, false};
    REQUIRE(Decide({.gpu_modified = {false, true}}, first_zero) == ConditionPath::InitialZero);
    REQUIRE(first_zero.reads == std::vector<size_t>{0});
    Memory nonzero;
    REQUIRE(Decide({.gpu_modified = {false, true}}, nonzero) == ConditionPath::ResolveCompare);
    REQUIRE(nonzero.reads == std::vector<size_t>{0});
}
