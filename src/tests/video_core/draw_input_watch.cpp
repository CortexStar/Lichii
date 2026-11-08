// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <initializer_list>
#include <map>
#include <set>
#include <span>

#include <catch2/catch_test_macros.hpp>

#include "common/input_watch.h"
#include "video_core/draw_input_watch.h"

namespace {
using VideoCommon::DrawInputWatch;
using Use = DrawInputWatch::Use;

/// The device pages' extra cached counts, and the pages the GPU writes.
struct FakeMemory {
    std::map<u64, int> held;
    std::set<u64> gpu_pages;

    static void Protect(void* context, DAddr address, u64 size, s32 delta) {
        auto& self = *static_cast<FakeMemory*>(context);
        for (u64 page = address >> 12; page < (address + size + 0xfff) >> 12; ++page) {
            self.held[page] += delta;
        }
    }
    static bool GpuPage(void* context, DAddr page) {
        return static_cast<FakeMemory*>(context)->gpu_pages.contains(page >> 12);
    }
};

struct Fixture {
    FakeMemory memory;
    DrawInputWatch watch;
    DrawInputWatch::Snapshot snapshot;

    Fixture() {
        watch.Bind(&FakeMemory::Protect, &FakeMemory::GpuPage, &memory);
    }
    ~Fixture() {
        watch.Unbind();
    }

    bool Capture(std::initializer_list<DrawInputWatch::Range> ranges, u64 frame = 1) {
        const u64 writes = watch.GuestWrites();
        return watch.Capture(std::span(ranges.begin(), ranges.size()), frame, writes, snapshot);
    }
};

// Distinct pages per test: the guest side's bucket table is process-wide.
constexpr DAddr HandlePage = 0x7'1000'0000;
} // Anonymous namespace

TEST_CASE("Draw input watch trusts a page only from the draw after it was armed",
          "[draw_input_watch]") {
    Fixture f;
    const DrawInputWatch::Range handle{HandlePage + 0x40, 4, Use::Handles};
    // The draw read the page before it was protected.
    REQUIRE_FALSE(f.Capture({handle}));
    REQUIRE(f.memory.held[HandlePage >> 12] == 1);
    REQUIRE(f.Capture({handle}));
    REQUIRE(f.watch.Unchanged(f.snapshot));
    // Releasing restores the cached count.
    f.watch.Unbind();
    REQUIRE(f.memory.held[HandlePage >> 12] == 0);
}

TEST_CASE("Draw input watch sees guest writes to armed pages", "[draw_input_watch]") {
    Fixture f;
    const DAddr page = HandlePage + 0x10000;
    const DrawInputWatch::Range handle{page + 0x100, 4, Use::Handles};
    REQUIRE_FALSE(f.Capture({handle}));
    REQUIRE(f.Capture({handle}));
    // A write elsewhere on the page: the whole page is suspect (one epoch per bucket).
    REQUIRE(Common::InputWatch::NoteWrite(page + 0x800, 4));
    REQUIRE_FALSE(f.watch.Unchanged(f.snapshot));
    // The sweep releases the broken page and waits before arming it again.
    f.watch.Sweep(10);
    REQUIRE(f.memory.held[page >> 12] == 0);
    REQUIRE_FALSE(f.Capture({handle}, 10));
    REQUIRE(f.memory.held[page >> 12] == 0);
    const u64 later = 10 + DrawInputWatch::RearmDelay(1);
    REQUIRE_FALSE(f.Capture({handle}, later)); // armed again now
    REQUIRE(f.memory.held[page >> 12] == 1);
    REQUIRE(f.Capture({handle}, later));
}

TEST_CASE("Draw input watch rejects a capture spanning a guest write", "[draw_input_watch]") {
    Fixture f;
    const DAddr page = HandlePage + 0x20000;
    const DrawInputWatch::Range handle{page, 4, Use::Handles};
    REQUIRE_FALSE(f.Capture({handle}));
    const u64 before = f.watch.GuestWrites();
    REQUIRE(Common::InputWatch::NoteWrite(page, 4)); // while the draw read it
    const std::array ranges{handle};
    REQUIRE_FALSE(f.watch.Capture(ranges, 1, before, f.snapshot));
}

TEST_CASE("Draw input watch tracks host writes per granule", "[draw_input_watch]") {
    Fixture f;
    const DAddr page = HandlePage + 0x30000;
    const DrawInputWatch::Range handle{page + 0x40, 4, Use::Handles};
    REQUIRE(DrawInputWatch::GranuleSlot((page + 0x40) >> 6) !=
            DrawInputWatch::GranuleSlot((page + 0x400) >> 6));
    REQUIRE_FALSE(f.Capture({handle}));
    REQUIRE(f.Capture({handle}));
    // Constant buffer data written next to the handle, in another granule: still unchanged.
    f.watch.NoteHostWrite(page + 0x400, 64);
    REQUIRE(f.watch.Unchanged(f.snapshot));
    // Over the handle's granule.
    f.watch.NoteHostWrite(page + 0x44, 4);
    REQUIRE_FALSE(f.watch.Unchanged(f.snapshot));
    REQUIRE(f.Capture({handle}));
    // Large host writes (unmaps, big copies) change everything.
    f.watch.NoteHostWrite(0, DrawInputWatch::MaxGranuleWrite + 1);
    REQUIRE_FALSE(f.watch.Unchanged(f.snapshot));
}

TEST_CASE("Draw input watch never arms GPU-written pages", "[draw_input_watch]") {
    Fixture f;
    const DAddr page = HandlePage + 0x40000;
    const DrawInputWatch::Range pool{page, 32, Use::Pools};
    f.memory.gpu_pages.insert(page >> 12);
    REQUIRE_FALSE(f.Capture({pool}));
    REQUIRE_FALSE(f.Capture({pool}));
    REQUIRE(f.memory.held[page >> 12] == 0);
}

TEST_CASE("Draw input watch drops snapshots of pages that become GPU-written",
          "[draw_input_watch]") {
    Fixture f;
    const DAddr page = HandlePage + 0x50000;
    const DrawInputWatch::Range storage{page, 16, Use::Storage};
    REQUIRE_FALSE(f.Capture({storage}));
    REQUIRE(f.Capture({storage}));
    f.memory.gpu_pages.insert(page >> 12);
    REQUIRE_FALSE(f.watch.Unchanged(f.snapshot));
    f.watch.Sweep(2);
    REQUIRE(f.memory.held[page >> 12] == 0);
}

TEST_CASE("Draw input watch releases unmapped pages", "[draw_input_watch]") {
    Fixture f;
    const DAddr page = HandlePage + 0x60000;
    const DrawInputWatch::Range handle{page, 4, Use::Handles};
    REQUIRE_FALSE(f.Capture({handle}));
    REQUIRE(f.Capture({handle}));
    f.watch.ReleaseRange(page, 0x1000);
    REQUIRE(f.memory.held[page >> 12] == 0);
    REQUIRE_FALSE(f.watch.Unchanged(f.snapshot));
}

TEST_CASE("Draw input watch releases pages inside a large unmapped range",
          "[draw_input_watch]") {
    Fixture f;
    const DAddr inside = HandlePage + 0x70000;
    const DAddr outside = HandlePage + 0x80000000;
    const DrawInputWatch::Range a{inside, 4, Use::Handles};
    const DrawInputWatch::Range b{outside, 4, Use::Handles};
    REQUIRE_FALSE(f.Capture({a, b}));
    // More pages than the watch holds: the release walks the held pages instead.
    f.watch.ReleaseRange(HandlePage, 0x40000000);
    REQUIRE(f.memory.held[inside >> 12] == 0);
    REQUIRE(f.memory.held[outside >> 12] == 1);
}

TEST_CASE("Draw input watch re-arm delay grows to 64 frames", "[draw_input_watch]") {
    REQUIRE(DrawInputWatch::RearmDelay(0) == 1);
    REQUIRE(DrawInputWatch::RearmDelay(1) == 2);
    REQUIRE(DrawInputWatch::RearmDelay(6) == 64);
    REQUIRE(DrawInputWatch::RearmDelay(40) == 64);
}
