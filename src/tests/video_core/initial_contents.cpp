// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/initial_contents.h"

using VideoCommon::ChooseInitialUpload;
using VideoCommon::InitialUpload;
using VideoCommon::NewImageContents;

namespace {
enum class FirstUse { Sample, Draw, PartialClear, FullClear };

/// Whether every texel is defined when the first use reads or keeps any of them, following the
/// texture cache: Now uploads before the join copies, AtFirstUse uploads at the first use unless
/// it is a full clear, Never has a copy writing every texel.
bool DefinedAtFirstUse(const NewImageContents& image, bool copies_cover_some, FirstUse use) {
    switch (ChooseInitialUpload(image)) {
    case InitialUpload::Never:
        return image.copy_writes_whole;
    case InitialUpload::Now:
        return true;
    case InitialUpload::AtFirstUse:
        // The deferred upload would land over join copies: there must be none.
        if (image.has_join_copies || copies_cover_some) {
            return false;
        }
        return true; // a full clear writes all, anything else uploads first
    }
    return false;
}
} // namespace

TEST_CASE("New images are never used with undefined texels", "[video_core][texture_cache]") {
    for (int bits = 0; bits < 16; ++bits) {
        const NewImageContents image{
            .copy_writes_whole = (bits & 1) != 0,
            .binding_clear_covers = (bits & 2) != 0 && (bits & 4) == 0,
            .has_join_copies = (bits & 4) != 0 || (bits & 1) != 0,
            .stale_guest_memory = (bits & 8) != 0,
        };
        for (const FirstUse use :
             {FirstUse::Sample, FirstUse::Draw, FirstUse::PartialClear, FirstUse::FullClear}) {
            INFO("bits " << bits << " use " << static_cast<int>(use));
            CHECK(DefinedAtFirstUse(image, image.has_join_copies, use));
        }
    }
}

TEST_CASE("Stale guest memory still uploads under join copies", "[video_core][texture_cache]") {
    // The Load menu case: a new image partly covered by an older one's copy, on stale pages.
    STATIC_REQUIRE(ChooseInitialUpload({.copy_writes_whole = false,
                                        .binding_clear_covers = false,
                                        .has_join_copies = true,
                                        .stale_guest_memory = true}) == InitialUpload::Now);
    // Alone on stale pages: the first use decides.
    STATIC_REQUIRE(ChooseInitialUpload({.copy_writes_whole = false,
                                        .binding_clear_covers = false,
                                        .has_join_copies = false,
                                        .stale_guest_memory = true}) == InitialUpload::AtFirstUse);
    STATIC_REQUIRE(ChooseInitialUpload({.copy_writes_whole = true,
                                        .binding_clear_covers = false,
                                        .has_join_copies = true,
                                        .stale_guest_memory = true}) == InitialUpload::Never);
    STATIC_REQUIRE(ChooseInitialUpload({.copy_writes_whole = false,
                                        .binding_clear_covers = false,
                                        .has_join_copies = false,
                                        .stale_guest_memory = false}) == InitialUpload::Now);
}
