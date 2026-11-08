// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/render_pass_barrier_tracker.h"

namespace {
using Vulkan::RenderPassBarrierTracker;

VkImage Image(u64 id) {
    VkImage image{};
    const u64 handle = id * 0x40;
    std::memcpy(&image, &handle, sizeof(image));
    return image;
}

const std::array SceneTargets{Image(1), Image(2)}; // colour + depth
const std::array BloomTarget{Image(3)};
const std::array HudTarget{Image(4)};
} // namespace

TEST_CASE("Render pass barrier is deferred past unrelated passes", "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    REQUIRE_FALSE(tracker.Pending());
    REQUIRE_FALSE(tracker.BeginNeedsBarrier(SceneTargets));

    REQUIRE_FALSE(tracker.EndPass(SceneTargets, true));
    REQUIRE(tracker.Pending());
    // A pass on other attachments whose draws sample other images.
    REQUIRE_FALSE(tracker.BeginNeedsBarrier(HudTarget));
    const std::array font{Image(10)};
    REQUIRE_FALSE(tracker.DrawNeedsBarrier(font, false));
    tracker.NoteDraw(font);
    REQUIRE_FALSE(tracker.EndPass(HudTarget, true));
    REQUIRE(tracker.Pending());
}

TEST_CASE("Render pass barrier comes before a read of a pending attachment",
          "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    REQUIRE_FALSE(tracker.EndPass(SceneTargets, true));
    REQUIRE_FALSE(tracker.BeginNeedsBarrier(BloomTarget));
    // The bloom pass samples the scene colour: read after write.
    const std::array scene_colour{Image(1)};
    REQUIRE(tracker.DrawNeedsBarrier(scene_colour, false));
    tracker.Clear();
    REQUIRE_FALSE(tracker.Pending());
    REQUIRE_FALSE(tracker.DrawNeedsBarrier(scene_colour, false));
}

TEST_CASE("Render pass barrier comes before a pending attachment is drawn to again",
          "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    REQUIRE_FALSE(tracker.EndPass(SceneTargets, true));
    REQUIRE_FALSE(tracker.EndPass(HudTarget, true));
    // Write after write and load after write on the scene targets, depth included.
    const std::array depth_only{Image(2)};
    REQUIRE(tracker.BeginNeedsBarrier(depth_only));
    REQUIRE(tracker.BeginNeedsBarrier(SceneTargets));
}

TEST_CASE("Render pass barrier comes before an attachment write over an earlier read",
          "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    // The scene pass samples the shadow map, then the next pass renders a new shadow map.
    const std::array shadow_map{Image(20)};
    tracker.NoteDraw(shadow_map);
    REQUIRE_FALSE(tracker.EndPass(SceneTargets, true));
    REQUIRE(tracker.BeginNeedsBarrier(shadow_map));
}

TEST_CASE("Render pass barrier comes before storage and transform feedback writes",
          "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    const std::array<VkImage, 0> none{};
    REQUIRE_FALSE(tracker.DrawNeedsBarrier(none, true)); // nothing deferred
    REQUIRE_FALSE(tracker.EndPass(SceneTargets, true));
    REQUIRE(tracker.DrawNeedsBarrier(none, true));
    REQUIRE_FALSE(tracker.DrawNeedsBarrier(none, false));
}

TEST_CASE("Render pass barrier is recorded at once for untracked passes and full sets",
          "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    REQUIRE(tracker.EndPass(SceneTargets, false));
    tracker.Clear();

    std::vector<VkImage> many;
    for (u64 id = 100; id < 400; ++id) {
        many.push_back(Image(id));
    }
    tracker.NoteDraw(many);
    REQUIRE(tracker.EndPass(HudTarget, true));
    tracker.Clear();
    REQUIRE_FALSE(tracker.EndPass(HudTarget, true));
    REQUIRE_FALSE(tracker.BeginNeedsBarrier(BloomTarget));
}

TEST_CASE("A remembered image set skips the check while nothing changed",
          "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    const std::array font{Image(10)};
    const std::array scene_color{Image(1)};

    // Nothing remembered yet.
    REQUIRE_FALSE(tracker.Remembers(font));
    REQUIRE_FALSE(tracker.Current());
    tracker.NoteDraw(font);
    tracker.Remember(0, font);
    REQUIRE(tracker.Remembers(font));
    REQUIRE(tracker.Current());
    REQUIRE_FALSE(tracker.Remembers(scene_color));

    // A pass end changes what the set has to be checked against.
    REQUIRE_FALSE(tracker.EndPass(SceneTargets, true));
    REQUIRE(tracker.Remembers(font));
    REQUIRE_FALSE(tracker.Current());
    REQUIRE_FALSE(tracker.DrawNeedsBarrier(tracker.RememberedImages(), false));
    tracker.NoteDraw(tracker.RememberedImages());
    tracker.RememberAgain();
    REQUIRE(tracker.Current());

    // A barrier does too.
    REQUIRE(tracker.DrawNeedsBarrier(scene_color, false));
    tracker.Clear();
    REQUIRE_FALSE(tracker.Current());
}

TEST_CASE("Image sets named by a generation are remembered by it", "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    const std::array table_images{Image(10), Image(11)};
    tracker.NoteDraw(table_images);
    tracker.Remember(7, table_images);
    REQUIRE(tracker.Remembers(u64{7}));
    REQUIRE_FALSE(tracker.Remembers(u64{8}));
    // Generation 0 means "named by its images"; a generation-named set never matches by images.
    REQUIRE_FALSE(tracker.Remembers(u64{0}));
    REQUIRE_FALSE(tracker.Remembers(table_images));
    // The tracker keeps the images, so a reuse after a pass end can be checked in full.
    REQUIRE_FALSE(tracker.EndPass(std::array{Image(11)}, true));
    REQUIRE(tracker.Remembers(u64{7}));
    REQUIRE_FALSE(tracker.Current());
    REQUIRE(tracker.DrawNeedsBarrier(tracker.RememberedImages(), false));
}

TEST_CASE("A remembered set keeps the reads the next pass begin checks",
          "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    const std::array bloom_input{Image(3)};
    tracker.NoteDraw(bloom_input);
    tracker.Remember(0, bloom_input);
    REQUIRE_FALSE(tracker.EndPass(SceneTargets, true));
    // A skipped NoteDraw loses no read: the bloom pass writing what an earlier draw sampled still
    // needs the barrier.
    REQUIRE(tracker.BeginNeedsBarrier(BloomTarget));
}

TEST_CASE("Image sets of any size are remembered", "[video_core][pass_barrier]") {
    RenderPassBarrierTracker tracker;
    std::vector<VkImage> many;
    for (u64 i = 0; i < 100; ++i) {
        many.push_back(Image(100 + i));
    }
    tracker.NoteDraw(many);
    tracker.Remember(0, many);
    REQUIRE(tracker.Remembers(many));
    many.pop_back();
    REQUIRE_FALSE(tracker.Remembers(many));
}
