// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/gpu_crumb_log.h"

namespace {
using namespace Vulkan;

bool Has(const std::vector<std::string>& lines, std::string_view part) {
    for (const std::string& line : lines) {
        if (line.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}
} // Anonymous namespace

TEST_CASE("GPU crumbs: the first unfinished dispatch inside a hung submit",
          "[video_core][diagnostics]") {
    CrumbLog log;
    const u32 submit = log.Add({.kind = CrumbKind::Submit, .submit = 1});
    const u32 pass = log.Add({.kind = CrumbKind::Pass, .submit = 1, .size = {1280, 720, 1}});
    const u32 dispatch1 =
        log.Add({.kind = CrumbKind::Work, .submit = 1, .a = 0x1111, .size = {40, 23, 1}});
    const u32 dispatch2 =
        log.Add({.kind = CrumbKind::Work, .submit = 1, .a = 0x2222, .size = {8, 8, 1}});
    log.SetSubmitTick(submit, 77);

    // The pass finished and the first dispatch; the second one started and never ended.
    std::array<std::array<u32, 2>, NumCrumbKinds> markers{};
    markers[static_cast<size_t>(CrumbKind::Submit)] = {submit, 0};
    markers[static_cast<size_t>(CrumbKind::Pass)] = {pass, pass};
    markers[static_cast<size_t>(CrumbKind::Work)] = {dispatch2, dispatch1};
    const auto lines = log.Report(markers);

    CHECK(Has(lines, "GPU crumbs: last finished submit: none"));
    CHECK(Has(lines, "first unfinished submit: #1 submit 1 (tick 77) (started)"));
    CHECK(Has(lines, "every recorded pass finished"));
    CHECK(Has(lines, "last finished draw/dispatch: #3 dispatch 0000000000001111 groups 40x23x1"));
    CHECK(Has(lines, "first unfinished draw/dispatch: #4 dispatch 0000000000002222 groups 8x8x1 "
                     "in submit 1 (started)"));
}

TEST_CASE("GPU crumbs: unstarted work and draws", "[video_core][diagnostics]") {
    CrumbLog log;
    log.Add({.kind = CrumbKind::Submit, .submit = 1});
    const u32 draw =
        log.Add({.kind = CrumbKind::Work, .draw = true, .submit = 1, .a = 0xa, .b = 0xb});
    log.Add({.kind = CrumbKind::Work, .submit = 1, .a = 0xc});
    std::array<std::array<u32, 2>, NumCrumbKinds> markers{};
    const auto lines = log.Report(markers);
    CHECK(Has(lines, "first unfinished draw/dispatch: #2 draw 000000000000000a 000000000000000b "
                     "in submit 1 (not started)"));
    CHECK(draw == 2);
}

TEST_CASE("GPU crumbs: ids older than the log are named as such", "[video_core][diagnostics]") {
    CrumbLog log;
    for (u32 i = 0; i < CrumbLog::Capacity + 10; ++i) {
        log.Add({.kind = CrumbKind::Pass, .submit = 1, .size = {1, 1, 1}});
    }
    std::array<std::array<u32, 2>, NumCrumbKinds> markers{};
    markers[static_cast<size_t>(CrumbKind::Pass)] = {5, 5};
    const auto lines = log.Report(markers);
    CHECK(Has(lines, "last finished pass: #5 (no longer in the log)"));
    CHECK(Has(lines, "first unfinished pass: #11 pass"));
}
