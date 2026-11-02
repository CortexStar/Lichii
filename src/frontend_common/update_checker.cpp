// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <charconv>
#include <optional>
#include <string_view>

#include "common/logging.h"
#include "common/net/net.h"
#include "common/scm_rev.h"
#include "update_checker.h"

namespace UpdateChecker {

namespace {

constexpr u32 max_changes = 100;

// Release tags are <channel>-build-<number>, where the channel is the branch CI built.
struct BuildTag {
    std::string_view channel;
    u64 number;
};

std::optional<BuildTag> ParseBuildTag(std::string_view tag) {
    constexpr std::string_view marker = "-build-";
    const auto pos = tag.rfind(marker);
    if (pos == std::string_view::npos || pos == 0)
        return std::nullopt;

    const auto digits = tag.substr(pos + marker.size());
    u64 number{};
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
        return std::nullopt;

    return BuildTag{tag.substr(0, pos), number};
}

} // namespace

Result Check() {
    const auto current = ParseBuildTag(Common::g_build_release_tag);
    if (!current)
        return Failure::NotReleaseBuild;

    const auto releases = Common::Net::GetReleases();
    if (!releases)
        return Failure::Unreachable;

    std::optional<Common::Net::Release> latest;
    u64 latest_number = current->number;
    for (const auto& release : *releases) {
        const auto tag = ParseBuildTag(release.tag);
        // Push builds only publish some platforms, so skip releases this build cannot download.
        if (tag && tag->channel == current->channel && tag->number > latest_number &&
            release.GetPlatformAsset()) {
            latest = release;
            latest_number = tag->number;
        }
    }

    if (!latest)
        return UpToDate{};

    LOG_INFO(Frontend, "Update available: {}", latest->tag);

    Update update{
        .release = *latest,
        .changes = {},
        .changes_complete = false,
        .compare_url = Common::Net::GetCompareUrl(Common::g_scm_rev, latest->commit),
    };

    if (auto commits = Common::Net::GetCommits(latest->commit, max_changes)) {
        for (auto& commit : *commits) {
            if (commit.sha == Common::g_scm_rev) {
                update.changes_complete = true;
                break;
            }
            update.changes.push_back(std::move(commit));
        }
    }

    return update;
}

} // namespace UpdateChecker
