// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "common/input_watch.h"
#include "common/logging.h"
#include "video_core/draw_input_watch.h"

namespace VideoCommon {

namespace {
using Common::InputWatch::GetTable;
constexpr u64 PageSize = u64{1} << Common::InputWatch::PageBits;
} // Anonymous namespace

DrawInputWatch::DrawInputWatch()
    : granule_versions{std::make_unique<std::atomic<u32>[]>(NumGranuleSlots)} {}

DrawInputWatch::~DrawInputWatch() = default;

void DrawInputWatch::Bind(ProtectFn protect_, GpuPageFn gpu_page_, void* context_) {
    std::scoped_lock lock{mutex};
    protect = protect_;
    gpu_page = gpu_page_;
    context = context_;
}

u32 DrawInputWatch::GranuleSlot(u64 granule) {
    return static_cast<u32>((granule * 0x9E3779B97F4A7C15ULL) >> 48) &
           static_cast<u32>(NumGranuleSlots - 1);
}

u64 DrawInputWatch::RearmDelay(u32 breaks) {
    // Pages the guest writes now and then are armed again soon; pages it keeps writing (data
    // next to the handles, say) wait up to 64 frames, so their writes rarely take the slow path.
    return u64{1} << (std::min)(breaks, 6U);
}

bool DrawInputWatch::WatchUniforms() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_DRAW_WATCH_UBO");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void DrawInputWatch::NoteHostWrite(DAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    if (size > MaxGranuleWrite) {
        host_epoch.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    const u64 last = (address + size - 1) >> GranuleBits;
    for (u64 granule = address >> GranuleBits; granule <= last; ++granule) {
        granule_versions[GranuleSlot(granule)].fetch_add(1, std::memory_order_acq_rel);
    }
}

u64 DrawInputWatch::GuestWrites() const {
    return GetTable().writes.load(std::memory_order_acquire);
}

int DrawInputWatch::Arm(u64 page, Use use, u64 frame) {
    auto& table = GetTable();
    Page& state = pages[page];
    state.uses |= static_cast<u8>(1U << static_cast<u32>(use));
    if (state.armed) {
        return 1;
    }
    if (frame < state.rearm_frame || protect == nullptr ||
        (gpu_page != nullptr && gpu_page(context, page * PageSize))) {
        ++stats.untrusted;
        return -1;
    }
    const size_t bucket = Common::InputWatch::Bucket(page);
    // Armed before protected: a write that traps on the page finds the bucket armed.
    table.armed[bucket].fetch_add(1, std::memory_order_acq_rel);
    protect(context, page * PageSize, PageSize, 1);
    state.armed = true;
    state.armed_epoch = table.epoch[bucket].load(std::memory_order_acquire);
    ++stats.arms;
    ++stats.armed_pages[static_cast<size_t>(use)];
    return 0;
}

void DrawInputWatch::Release(u64 page, Page& state, bool broken) {
    if (!state.armed) {
        return;
    }
    auto& table = GetTable();
    const size_t bucket = Common::InputWatch::Bucket(page);
    protect(context, page * PageSize, PageSize, -1);
    table.armed[bucket].fetch_sub(1, std::memory_order_acq_rel);
    // Snapshots holding the page see it go.
    table.epoch[bucket].fetch_add(1, std::memory_order_acq_rel);
    state.armed = false;
    ++stats.releases;
    for (u32 use = 0; use < static_cast<u32>(Use::Count); ++use) {
        if ((state.uses >> use) & 1U) {
            auto& armed = stats.armed_pages[use];
            armed -= armed != 0 ? 1 : 0;
            if (broken) {
                ++stats.breaks[use];
            }
        }
    }
    if (broken) {
        ++state.breaks;
    }
}

bool DrawInputWatch::Capture(std::span<const Range> ranges, u64 frame, u64 guest_writes_before,
                             Snapshot& out) {
    auto& table = GetTable();
    out.pages.clear();
    out.granules.clear();
    out.host_epoch = host_epoch.load(std::memory_order_acquire);
    bool trusted = true;
    {
        std::scoped_lock lock{mutex};
        for (const Range& range : ranges) {
            if (range.size == 0) {
                continue;
            }
            const u64 last = (range.address + range.size - 1) / PageSize;
            for (u64 page = range.address / PageSize; page <= last; ++page) {
                const bool listed = std::ranges::any_of(
                    out.pages, [page](const PageInput& input) { return input.page == page; });
                if (listed) {
                    continue;
                }
                // Armed now (0): the draw read the page before it was protected.
                if (Arm(page, range.use, frame) != 1) {
                    trusted = false;
                    continue;
                }
                const size_t bucket = Common::InputWatch::Bucket(page);
                out.pages.push_back({page, static_cast<u32>(bucket),
                                     table.epoch[bucket].load(std::memory_order_acquire)});
            }
        }
    }
    if (!trusted || table.writes.load(std::memory_order_acquire) != guest_writes_before) {
        return false;
    }
    for (const Range& range : ranges) {
        if (range.size == 0) {
            continue;
        }
        const u64 last = (range.address + range.size - 1) >> GranuleBits;
        for (u64 granule = range.address >> GranuleBits; granule <= last; ++granule) {
            const u32 slot = GranuleSlot(granule);
            const bool listed = std::ranges::any_of(
                out.granules, [slot](const GranuleInput& input) { return input.slot == slot; });
            if (!listed) {
                out.granules.push_back(
                    {slot, granule_versions[slot].load(std::memory_order_acquire)});
            }
        }
    }
    return true;
}

void DrawInputWatch::ArmOnly(std::span<const Range> ranges, u64 frame) {
    std::scoped_lock lock{mutex};
    for (const Range& range : ranges) {
        if (range.size == 0) {
            continue;
        }
        const u64 last = (range.address + range.size - 1) / PageSize;
        for (u64 page = range.address / PageSize; page <= last; ++page) {
            (void)Arm(page, range.use, frame);
        }
    }
}

bool DrawInputWatch::Unchanged(const Snapshot& snapshot) const {
    if (host_epoch.load(std::memory_order_acquire) != snapshot.host_epoch) {
        return false;
    }
    const auto& table = GetTable();
    for (const PageInput& page : snapshot.pages) {
        if (table.epoch[page.bucket].load(std::memory_order_acquire) != page.epoch) {
            return false;
        }
        // The GPU may have started writing it (a render target or storage buffer placed there):
        // its bytes then change with no signal here.
        if (gpu_page != nullptr && gpu_page(context, page.page * PageSize)) {
            return false;
        }
    }
    for (const GranuleInput& granule : snapshot.granules) {
        if (granule_versions[granule.slot].load(std::memory_order_acquire) != granule.version) {
            return false;
        }
    }
    return true;
}

void DrawInputWatch::Sweep(u64 frame) {
    const auto& table = GetTable();
    current_frame = frame;
    std::scoped_lock lock{mutex};
    for (auto it = pages.begin(); it != pages.end();) {
        auto& [page, state] = *it;
        if (!state.armed) {
            // Forgotten a minute after it could be armed again.
            if (state.rearm_frame != ~u64{0} && frame > state.rearm_frame + 3600) {
                it = pages.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        const size_t bucket = Common::InputWatch::Bucket(page);
        const bool broken =
            table.epoch[bucket].load(std::memory_order_acquire) != state.armed_epoch;
        const bool gpu_written = gpu_page != nullptr && gpu_page(context, page * PageSize);
        if (broken || gpu_written) {
            Release(page, state, broken);
            state.rearm_frame = gpu_written ? ~u64{0} : frame + RearmDelay(state.breaks);
        }
        ++it;
    }
}

void DrawInputWatch::Unbind() {
    std::scoped_lock lock{mutex};
    if (protect != nullptr) {
        for (auto& [page, state] : pages) {
            Release(page, state, false);
        }
    }
    pages.clear();
    protect = nullptr;
    gpu_page = nullptr;
    context = nullptr;
}

void DrawInputWatch::ReleaseRange(DAddr address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 first = address / PageSize;
    const u64 last = (address + size - 1) / PageSize;
    std::scoped_lock lock{mutex};
    if (protect == nullptr || pages.empty()) {
        return;
    }
    if (last - first < pages.size()) {
        // Small ranges (every invalidation comes here): look their pages up.
        for (u64 page = first; page <= last; ++page) {
            if (const auto it = pages.find(page); it != pages.end()) {
                Release(it->first, it->second, false);
                pages.erase(it);
            }
        }
        return;
    }
    for (auto it = pages.begin(); it != pages.end();) {
        if (it->first >= first && it->first <= last) {
            Release(it->first, it->second, false);
            it = pages.erase(it);
        } else {
            ++it;
        }
    }
}

DrawInputWatch::Stats DrawInputWatch::TakeStats() {
    const auto& table = GetTable();
    std::scoped_lock lock{mutex};
    Stats result = stats;
    const u64 writes = table.writes.load(std::memory_order_relaxed);
    const u64 handler = table.handler_ns.load(std::memory_order_relaxed);
    result.guest_writes = writes - std::exchange(last_guest_writes, writes);
    result.handler_ns = handler - std::exchange(last_handler_ns, handler);
    // Counts since the last call; the armed pages are a current total.
    stats.breaks.fill(0);
    stats.arms = 0;
    stats.releases = 0;
    stats.untrusted = 0;
    return result;
}

void DrawInputWatch::Report(double seconds) {
    const Stats watch = TakeStats();
    const auto per_second = [seconds](u64 value) {
        return seconds > 0.0 ? static_cast<double>(value) / seconds : 0.0;
    };
    const auto use = [](Use u) { return static_cast<size_t>(u); };
    LOG_INFO(
        HW_GPU,
        "Draw input watch: armed pages: handles {}, pools {}, storage {}, uniform {}; guest "
        "writes to watched pages {:.0f}/s, {:.1f} us/s in the write handler; broken by "
        "writes/s: handles {:.1f}, pools {:.1f}, storage {:.1f}, uniform {:.1f}; arms {:.1f}/s, "
        "releases {:.1f}/s, refused {:.1f}/s",
        watch.armed_pages[use(Use::Handles)], watch.armed_pages[use(Use::Pools)],
        watch.armed_pages[use(Use::Storage)], watch.armed_pages[use(Use::Uniform)],
        per_second(watch.guest_writes), per_second(watch.handler_ns) / 1000.0,
        per_second(watch.breaks[use(Use::Handles)]), per_second(watch.breaks[use(Use::Pools)]),
        per_second(watch.breaks[use(Use::Storage)]), per_second(watch.breaks[use(Use::Uniform)]),
        per_second(watch.arms), per_second(watch.releases), per_second(watch.untrusted));
}

DrawInputWatch& GetDrawInputWatch() {
    static DrawInputWatch watch;
    return watch;
}

} // namespace VideoCommon
