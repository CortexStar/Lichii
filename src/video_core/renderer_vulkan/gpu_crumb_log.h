// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"

namespace Vulkan {

/// What the GPU crumbs count, each kind with its own pair of marker slots (started, finished): a
/// pass's end marker comes after its draws', so one shared pair would not stay monotonic.
enum class CrumbKind : u8 {
    Submit,
    Pass,
    Work, ///< A guest dispatch, or with EDEN_NX2_GPU_CRUMBS=2 also a draw.
};
inline constexpr size_t NumCrumbKinds = 3;

struct Crumb {
    CrumbKind kind{};
    bool draw{};               ///< Work inside a render pass.
    u32 submit{};              ///< The submit it was recorded in.
    u64 a{};                   ///< Submit: GPU tick. Work: shader hash (vertex for a draw).
    u64 b{};                   ///< Work: fragment hash.
    std::array<u32, 3> size{}; ///< Pass: width, height. Dispatch: groups.
};

/// The host side of the GPU crumbs: what each marker id stands for. Ids count up from 1 across all
/// kinds; the GPU writes the id of the newest started and finished crumb of each kind.
class CrumbLog {
public:
    static constexpr u32 Capacity = 1u << 16;

    CrumbLog() : crumbs(Capacity) {}

    /// Records a crumb and returns its id (never 0).
    u32 Add(const Crumb& crumb) {
        std::scoped_lock lock{mutex};
        const u32 id = ++last_id;
        crumbs[id % Capacity] = crumb;
        return id;
    }

    /// Submits learn their tick when they end.
    void SetSubmitTick(u32 id, u64 tick) {
        std::scoped_lock lock{mutex};
        if (last_id - id < Capacity) {
            crumbs[id % Capacity].a = tick;
        }
    }

    /// The log lines for the marker values the GPU last wrote, per kind {started, finished}.
    [[nodiscard]] std::vector<std::string> Report(
        const std::array<std::array<u32, 2>, NumCrumbKinds>& markers) const {
        std::scoped_lock lock{mutex};
        std::vector<std::string> lines;
        lines.push_back(fmt::format("GPU crumbs: {} recorded", last_id));
        for (size_t kind = 0; kind < NumCrumbKinds; ++kind) {
            const u32 started = markers[kind][0];
            const u32 finished = markers[kind][1];
            const auto name = KindName(static_cast<CrumbKind>(kind));
            lines.push_back(fmt::format("GPU crumbs: last finished {}: {}", name,
                                        finished == 0 ? "none" : Describe(finished)));
            const std::optional<u32> unfinished = Next(static_cast<CrumbKind>(kind), finished);
            if (!unfinished) {
                lines.push_back(fmt::format("GPU crumbs: every recorded {} finished", name));
                continue;
            }
            lines.push_back(fmt::format("GPU crumbs: first unfinished {}: {} ({})", name,
                                        Describe(*unfinished),
                                        *unfinished <= started ? "started" : "not started"));
            if (started > *unfinished) {
                lines.push_back(
                    fmt::format("GPU crumbs: last started {}: {}", name, Describe(started)));
            }
        }
        return lines;
    }

private:
    static std::string_view KindName(CrumbKind kind) {
        switch (kind) {
        case CrumbKind::Submit:
            return "submit";
        case CrumbKind::Pass:
            return "pass";
        case CrumbKind::Work:
            return "draw/dispatch";
        }
        return "?";
    }

    /// The first crumb of `kind` after `id` still in the log. Call locked.
    [[nodiscard]] std::optional<u32> Next(CrumbKind kind, u32 id) const {
        const u32 oldest = last_id >= Capacity ? last_id - Capacity + 1 : 1;
        for (u32 next = (std::max)(id + 1, oldest); next <= last_id && next != 0; ++next) {
            if (crumbs[next % Capacity].kind == kind) {
                return next;
            }
        }
        return std::nullopt;
    }

    /// Call locked.
    [[nodiscard]] std::string Describe(u32 id) const {
        if (id > last_id || last_id - id >= Capacity) {
            return fmt::format("#{} (no longer in the log)", id);
        }
        const Crumb& crumb = crumbs[id % Capacity];
        switch (crumb.kind) {
        case CrumbKind::Submit:
            return fmt::format("#{} submit {} (tick {})", id, crumb.submit, crumb.a);
        case CrumbKind::Pass:
            return fmt::format("#{} pass {}x{} in submit {}", id, crumb.size[0], crumb.size[1],
                               crumb.submit);
        case CrumbKind::Work:
            if (crumb.draw) {
                return fmt::format("#{} draw {:016x} {:016x} in submit {}", id, crumb.a, crumb.b,
                                   crumb.submit);
            }
            return fmt::format("#{} dispatch {:016x} groups {}x{}x{} in submit {}", id, crumb.a,
                               crumb.size[0], crumb.size[1], crumb.size[2], crumb.submit);
        }
        return fmt::format("#{}", id);
    }

    mutable std::mutex mutex;
    std::vector<Crumb> crumbs;
    u32 last_id{};
};

} // namespace Vulkan
