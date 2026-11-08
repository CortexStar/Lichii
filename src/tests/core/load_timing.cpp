// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "core/load_timing.h"

namespace {

using Core::LoadTiming::Clock;
using Core::LoadTiming::Counters;
using Core::LoadTiming::LoadWindow;
using Core::LoadTiming::LoadWindowTracker;
using namespace std::chrono_literals;

struct Feed {
    LoadWindowTracker tracker;
    Clock::time_point now{};
    Counters counters{};

    std::optional<LoadWindow> Frame(Clock::duration after, u64 reads = 0) {
        now += after;
        counters.file_reads += reads;
        return tracker.OnFrame(now, counters);
    }

    /// Regular frames until the tracker reports a load or count frames went by.
    std::optional<LoadWindow> Regular(u32 count) {
        for (u32 i = 0; i < count; ++i) {
            if (auto load = Frame(16ms)) {
                return load;
            }
        }
        return std::nullopt;
    }
};

} // Anonymous namespace

TEST_CASE("LoadWindowTracker ignores regular frames and short hitches", "[core][load_timing]") {
    Feed feed;
    REQUIRE_FALSE(feed.Regular(100));
    REQUIRE_FALSE(feed.Frame(400ms));
    REQUIRE_FALSE(feed.Regular(LoadWindowTracker::RegularFrames + 10));
}

TEST_CASE("LoadWindowTracker reports a load of slow frames with its work", "[core][load_timing]") {
    Feed feed;
    REQUIRE_FALSE(feed.Regular(10));
    const auto begin = feed.now;
    // Loading frames at ~3 fps for 3 s, with file reads between them.
    for (int i = 0; i < 10; ++i) {
        REQUIRE_FALSE(feed.Frame(300ms, 100));
    }
    const auto end = feed.now;
    // Gameplay frames with reads of their own: not part of the load.
    REQUIRE_FALSE(feed.Regular(1));
    feed.counters.file_reads += 5;
    REQUIRE_FALSE(feed.Regular(LoadWindowTracker::RegularFrames - 2));
    const auto load = feed.Regular(1);
    REQUIRE(load);
    REQUIRE(load->begin == begin);
    REQUIRE(load->end == end);
    REQUIRE(load->frames == 10);
    REQUIRE(load->work.file_reads == 1000);
}

TEST_CASE("LoadWindowTracker keeps a load open across short runs of regular frames",
          "[core][load_timing]") {
    Feed feed;
    REQUIRE_FALSE(feed.Regular(5));
    const auto begin = feed.now;
    REQUIRE_FALSE(feed.Frame(2s));
    REQUIRE_FALSE(feed.Regular(LoadWindowTracker::RegularFrames - 1));
    REQUIRE_FALSE(feed.Frame(1s));
    const auto end = feed.now;
    const auto load = feed.Regular(LoadWindowTracker::RegularFrames);
    REQUIRE(load);
    REQUIRE(load->begin == begin);
    REQUIRE(load->end == end);
    REQUIRE(load->frames == LoadWindowTracker::RegularFrames + 1);
}

TEST_CASE("LoadWindowTracker counts the first stall after boot", "[core][load_timing]") {
    Feed feed;
    // The first frame has no predecessor, so it never opens a load by itself.
    REQUIRE_FALSE(feed.Frame(10s));
    REQUIRE_FALSE(feed.Frame(1500ms));
    const auto load = feed.Regular(LoadWindowTracker::RegularFrames);
    REQUIRE(load);
    REQUIRE(load->frames == 1);
    REQUIRE(load->end - load->begin == 1500ms);
}

TEST_CASE("Counters subtract field by field", "[core][load_timing]") {
    const Counters a{.file_reads = 5, .file_bytes = 100, .jit_blocks = 9, .jit_cache_clears = 2};
    const Counters b{.file_reads = 2, .file_bytes = 40, .jit_blocks = 4, .jit_cache_clears = 1};
    const Counters d = a - b;
    REQUIRE(d.file_reads == 3);
    REQUIRE(d.file_bytes == 60);
    REQUIRE(d.jit_blocks == 5);
    REQUIRE(d.jit_cache_clears == 1);
    REQUIRE(d.service_calls == 0);
    const Counters c{.host_decompressions = 7, .host_decompressed_bytes = 900};
    const Counters e{.host_decompressions = 3, .host_decompressed_bytes = 100};
    REQUIRE((c - e).host_decompressions == 4);
    REQUIRE((c - e).host_decompressed_bytes == 800);
}
