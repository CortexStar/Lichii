// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/hazard_tracker.h"

namespace {
using VideoCommon::HazardTracker;
using Decision = HazardTracker::Decision;
using Accesses = HazardTracker::Accesses;

constexpr u64 ImageA = 0x1000;
constexpr u64 ImageB = 0x2000;

Accesses Reads(u64 begin, u64 size) {
    Accesses a;
    a.ReadBuffer(begin, size);
    return a;
}
Accesses Writes(u64 begin, u64 size) {
    Accesses a;
    a.WriteBuffer(begin, size);
    return a;
}

/// A tracker whose last barrier preceded one dispatch with `first`.
HazardTracker After(const Accesses& first) {
    HazardTracker tracker;
    REQUIRE(tracker.Next(true, first) == Decision::OtherWork);
    return tracker;
}
} // namespace

TEST_CASE("The first dispatch and dispatches after other work get the barrier",
          "[video_core][hazard_tracker]") {
    HazardTracker tracker;
    CHECK(tracker.Next(false, Reads(0, 16)) == Decision::OtherWork);
    CHECK(tracker.Next(false, Reads(0, 16)) == Decision::NoBarrier);
    CHECK(tracker.Next(true, Reads(0, 16)) == Decision::OtherWork);
}

TEST_CASE("Independent dispatches run without a barrier", "[video_core][hazard_tracker]") {
    auto tracker = After(Writes(0x100, 0x100));
    // Reads and writes elsewhere, including the bytes right next to the written range.
    Accesses next = Reads(0x200, 0x100);
    next.WriteBuffer(0x0, 0x100);
    next.ReadImage(ImageA);
    next.WriteImage(ImageB);
    CHECK(tracker.Next(false, next) == Decision::NoBarrier);
    // Reading what both wrote is still a hazard after the skipped barrier.
    CHECK(tracker.Next(false, Reads(0x1ff, 1)) == Decision::ReadAfterWrite);
}

TEST_CASE("Buffer hazards are found by range overlap", "[video_core][hazard_tracker]") {
    SECTION("read after write") {
        auto tracker = After(Writes(0x100, 0x100));
        CHECK(tracker.Next(false, Reads(0x1ff, 4)) == Decision::ReadAfterWrite);
    }
    SECTION("write after write") {
        auto tracker = After(Writes(0x100, 0x100));
        CHECK(tracker.Next(false, Writes(0xf0, 0x20)) == Decision::WriteAfterWrite);
    }
    SECTION("write after read") {
        auto tracker = After(Reads(0x100, 0x100));
        CHECK(tracker.Next(false, Writes(0x180, 4)) == Decision::WriteAfterRead);
    }
    SECTION("read after read") {
        auto tracker = After(Reads(0x100, 0x100));
        CHECK(tracker.Next(false, Reads(0x100, 0x100)) == Decision::NoBarrier);
    }
}

TEST_CASE("Image hazards are found by handle", "[video_core][hazard_tracker]") {
    Accesses write_a;
    write_a.WriteImage(ImageA);
    Accesses read_a;
    read_a.ReadImage(ImageA);
    Accesses read_b;
    read_b.ReadImage(ImageB);
    {
        auto tracker = After(write_a);
        CHECK(tracker.Next(false, read_b) == Decision::NoBarrier);
        CHECK(tracker.Next(false, read_a) == Decision::ReadAfterWrite);
    }
    {
        auto tracker = After(write_a);
        CHECK(tracker.Next(false, write_a) == Decision::WriteAfterWrite);
    }
    {
        auto tracker = After(read_a);
        CHECK(tracker.Next(false, read_a) == Decision::NoBarrier);
        CHECK(tracker.Next(false, write_a) == Decision::WriteAfterRead);
    }
}

TEST_CASE("A barrier clears what was pending", "[video_core][hazard_tracker]") {
    auto tracker = After(Writes(0x100, 0x100));
    // This dispatch reads what the first wrote: barrier, and only its own reads stay pending.
    Accesses second = Reads(0x100, 0x10);
    second.WriteBuffer(0x1000, 0x10);
    CHECK(tracker.Next(false, second) == Decision::ReadAfterWrite);
    CHECK(tracker.Next(false, Writes(0x180, 0x10)) == Decision::NoBarrier);
    CHECK(tracker.Next(false, Reads(0x1000, 4)) == Decision::ReadAfterWrite);
}

TEST_CASE("Unknown accesses always get the barrier", "[video_core][hazard_tracker]") {
    auto tracker = After(Reads(0, 4));
    Accesses unknown;
    unknown.MarkUnknown();
    CHECK(tracker.Next(false, unknown) == Decision::Unknown);
    // The unknown dispatch stays pending until the next barrier.
    CHECK(tracker.Next(false, Reads(0x100, 4)) == Decision::Unknown);
    CHECK(tracker.Next(false, Reads(0x200, 4)) == Decision::NoBarrier);
}

TEST_CASE("Overflowing the access lists asks for the barrier", "[video_core][hazard_tracker]") {
    Accesses many;
    for (u64 i = 0; i < 65; ++i) {
        many.ReadBuffer(i * 0x100, 0x10);
    }
    auto tracker = After(Reads(0x100000, 4));
    CHECK(tracker.Next(false, many) == Decision::Unknown);

    // Pending lists fill up over a run of dispatches.
    HazardTracker run;
    REQUIRE(run.Next(true, Reads(0, 4)) == Decision::OtherWork);
    Decision last = Decision::NoBarrier;
    for (u64 i = 1; i < 300 && last == Decision::NoBarrier; ++i) {
        last = run.Next(false, Reads(i * 0x100, 4));
    }
    CHECK(last == Decision::Unknown);
}

TEST_CASE("Empty ranges and null images are not accesses", "[video_core][hazard_tracker]") {
    Accesses first;
    first.WriteBuffer(0x100, 0);
    first.WriteImage(0);
    auto tracker = After(first);
    Accesses next = Reads(0x100, 4);
    next.ReadImage(0);
    CHECK(tracker.Next(false, next) == Decision::NoBarrier);
}
