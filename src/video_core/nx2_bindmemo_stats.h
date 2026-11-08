// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_profile.h"

// Counts of why the texture binding memo and the pipeline lookup's skips hit or miss, logged
// every 10 s with EDEN_NX2_GPU_PROFILE.
namespace VideoCommon::Nx2BindMemo {

enum class Stat : u32 {
    // Binding lookups (one per texture/image binding of a draw or dispatch)
    Lookups,
    Hits,
    MissNoEntry, ///< no entry with this key (first use, evicted)
    MissChannel, ///< recorded on another channel
    MissPool,    ///< TIC/TSC pool address or limit moved
    MissEpoch,   ///< every memo dropped (rescale)
    MissImage,   ///< an event on its image (CPU write, alias added, deleted, remapped)
    MissAlias,   ///< an alias is newer than the image
    MissMap,     ///< GPU mappings changed (null and buffer views)
    MissTic,     ///< the guest TIC entry changed
    MissTsc,     ///< the guest TSC entry changed
    // Per draw or dispatch with bindings
    Draws,
    DrawsAllHit,
    DrawsPartial,
    DrawsAllMiss,
    DrawsResolvedAgain, ///< resolving the misses changed the texture cache: all resolved again
    DrawsAliasedHit,    ///< partial, with a hit of an image with aliases: all resolved
    BindingsResolved,   ///< bindings that went through FillImageViews
    // Records (one per binding resolved)
    Records,
    RecordsAliased,
    RefuseImageEvent, ///< an event on its image during the draw (created, joined, written)
    RefuseEpoch,      ///< rescale or channel change during the draw
    RefuseCpuModified,
    RefuseTable, ///< the descriptor tables no longer give the resolved ids
    RefuseOther,
    EvictedLive, ///< a record replaced an entry used this frame (set conflicts)
    // Pipeline lookups (PipelineCache::CurrentGraphicsPipeline)
    PipeCalls,
    PipeSkip,
    PipeSkipShadersSame, ///< of PipeSkip: shader registers written, same stage hashes
    PipeStateSameShadersChanged,
    PipeFlagValuesSame, ///< flag raised, every watched register as at the last Refresh
    PipeMissNotSetUp,
    PipeMissFlag,
    PipeMissVertexInput,
    PipeMissDrawState, ///< topology, engine hint or dual-source blending
    PipeMissValues,    ///< a register compared by value
    PipeMissSwizzles,
    PipeMissXfb,
    PipeMissKeyStale, ///< state unchanged, but the last lookup didn't resolve this key
    PipeNextSame,     ///< after Refresh: the key is the current pipeline's
    PipeNextTransition,
    PipeSlowPath,
    PipePatchBlend,       ///< only the blend attachments were redone
    PipePatchVertexInput, ///< only the attribute types were redone
    // Guest page translations (GuestPageCache)
    HandleWords,      ///< texture handle words read from a translated page
    HandleWordsSlow,  ///< read with MemoryManager::Read (page split, unmapped, shim, GPU-written)
    PageTranslations, ///< pages translated for handles, TIC and TSC entries
    Count,
};

inline std::array<std::atomic<u64>, static_cast<size_t>(Stat::Count)> g_stats{};
inline u64 g_stats_start{};

/// Counted by one thread at a time (the GPU back thread): a plain load and store, no locked add.
inline void Note(Stat stat, u64 amount = 1) {
    if (Nx2Profile::Enabled()) {
        auto& counter = g_stats[static_cast<size_t>(stat)];
        counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
    }
}

/// Logs and resets the counts every 10 s (called once per frame).
inline void Report() {
    if (!Nx2Profile::Enabled()) {
        return;
    }
    const u64 now = Nx2Profile::Now();
    if (g_stats_start == 0) {
        g_stats_start = now;
        return;
    }
    const u64 window = now - g_stats_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    std::array<u64, static_cast<size_t>(Stat::Count)> stats{};
    for (size_t i = 0; i < stats.size(); ++i) {
        stats[i] = g_stats[i].exchange(0, std::memory_order_relaxed);
    }
    const auto rate = [&](Stat stat) {
        return static_cast<double>(stats[static_cast<size_t>(stat)]) * 1e9 /
               static_cast<double>(window);
    };
    const auto share = [&](Stat part, Stat whole) {
        const u64 total = stats[static_cast<size_t>(whole)];
        return total == 0 ? 0.0
                          : 100.0 * static_cast<double>(stats[static_cast<size_t>(part)]) /
                                static_cast<double>(total);
    };
    LOG_INFO(
        HW_GPU,
        "Binding memo per second: lookups {:.0f}, hits {:.0f} ({:.1f}%) | misses: no entry "
        "{:.0f}, channel {:.0f}, pools {:.0f}, epoch {:.0f}, image event {:.0f}, newer alias "
        "{:.0f}, mappings {:.0f}, TIC changed {:.0f}, TSC changed {:.0f} | draws {:.0f}: all "
        "hit {:.0f}, partial {:.0f}, all missed {:.0f}, resolved again {:.0f}, aliased hit with "
        "misses {:.0f}; bindings "
        "resolved {:.0f} | records {:.0f} ({:.0f} aliased), refused: image event during the "
        "draw {:.0f}, epoch {:.0f}, CPU modified {:.0f}, table {:.0f}, other {:.0f} | live "
        "entries evicted {:.0f}",
        rate(Stat::Lookups), rate(Stat::Hits), share(Stat::Hits, Stat::Lookups),
        rate(Stat::MissNoEntry), rate(Stat::MissChannel), rate(Stat::MissPool),
        rate(Stat::MissEpoch), rate(Stat::MissImage), rate(Stat::MissAlias), rate(Stat::MissMap),
        rate(Stat::MissTic), rate(Stat::MissTsc), rate(Stat::Draws), rate(Stat::DrawsAllHit),
        rate(Stat::DrawsPartial), rate(Stat::DrawsAllMiss), rate(Stat::DrawsResolvedAgain),
        rate(Stat::DrawsAliasedHit), rate(Stat::BindingsResolved), rate(Stat::Records),
        rate(Stat::RecordsAliased), rate(Stat::RefuseImageEvent), rate(Stat::RefuseEpoch),
        rate(Stat::RefuseCpuModified), rate(Stat::RefuseTable), rate(Stat::RefuseOther),
        rate(Stat::EvictedLive));
    LOG_INFO(HW_GPU,
             "Pipeline lookup per second: calls {:.0f}, skipped {:.0f} ({:.1f}%; {:.0f} with the "
             "same shaders bound again, {:.0f} with the flag raised but equal values) | state "
             "unchanged, shaders changed {:.0f} | refreshed because: not set up {:.0f}, flag "
             "{:.0f}, vertex input {:.0f}, topology/engine/dual-source {:.0f}, "
             "compared registers {:.0f}, swizzles {:.0f}, transform feedback {:.0f}, key not "
             "resolved {:.0f} | then: same pipeline {:.0f}, transition {:.0f}, cache lookup {:.0f}",
             rate(Stat::PipeCalls), rate(Stat::PipeSkip), share(Stat::PipeSkip, Stat::PipeCalls),
             rate(Stat::PipeSkipShadersSame), rate(Stat::PipeFlagValuesSame),
             rate(Stat::PipeStateSameShadersChanged), rate(Stat::PipeMissNotSetUp),
             rate(Stat::PipeMissFlag), rate(Stat::PipeMissVertexInput),
             rate(Stat::PipeMissDrawState), rate(Stat::PipeMissValues),
             rate(Stat::PipeMissSwizzles), rate(Stat::PipeMissXfb), rate(Stat::PipeMissKeyStale),
             rate(Stat::PipeNextSame), rate(Stat::PipeNextTransition), rate(Stat::PipeSlowPath));
    LOG_INFO(HW_GPU,
             "Texture lookup per second: pipeline keys patched: blending {:.0f}, vertex input "
             "{:.0f} | handle words {:.0f} from pages, {:.0f} read slowly | pages translated "
             "{:.0f}",
             rate(Stat::PipePatchBlend), rate(Stat::PipePatchVertexInput), rate(Stat::HandleWords),
             rate(Stat::HandleWordsSlow), rate(Stat::PageTranslations));
    g_stats_start = now;
}

} // namespace VideoCommon::Nx2BindMemo
