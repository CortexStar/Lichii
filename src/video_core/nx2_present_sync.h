// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

// Ocarina of Time runs a frame ahead (PresentClientBuffer) and draws its dialogue box panel, hint
// strip and HUD from storage it rewrites every frame. When the GPU thread uploads that storage for
// a frame, the game has often written the next frame's data already, so the panel was missing in
// about half the frames (#286). At each present the ranges UI draws uploaded are copied
// (TakeUiSnapshot in nx2_gsv.h); until the GPU thread reaches that present's fence, its uploads of
// those ranges take the copy instead of guest memory. The GPU thread notes the ranges as UI draws
// bind them (Nx2NoteUiStorage), so a buffer is covered from the second frame it is drawn from.

/// Set by PresentClientBuffer while the game runs a frame ahead with the copy.
inline std::atomic<bool> g_nx2_ui_snapshot_enabled{false};

inline bool Nx2UiSnapshotEnabled() {
    return g_nx2_ui_snapshot_enabled.load(std::memory_order_relaxed);
}

/// Whether vertex program vs or fragment program fs is one of Ocarina of Time's UI programs (the
/// dialogue box panel's vertex and fragment shaders).
inline bool Nx2IsUiProgram(u64 vs, u64 fs) {
    constexpr u64 OcarinaUiPrograms[]{0xf45e32247832c053ULL, 0xfc8435529cbaf776ULL};
    return std::ranges::find(OcarinaUiPrograms, vs) != std::end(OcarinaUiPrograms) ||
           std::ranges::find(OcarinaUiPrograms, fs) != std::end(OcarinaUiPrograms);
}

/// Set on the GPU thread while a UI draw's pipeline binds its buffers.
inline thread_local bool t_nx2_ui_draw = false;
/// Set on the GPU thread while a UI draw's read-only storage buffer synchronizes: the CPU-written
/// ranges it uploads are the ones the game rewrites each frame (a binding can span megabytes of
/// which a frame rewrites a few pages; copying whole bindings took 18-24 MiB a present).
inline thread_local bool t_nx2_ui_storage_upload = false;

struct Nx2UiRange {
    u64 address{}; ///< Device address
    u64 size{};
    u64 tag{}; ///< In ranges: the present that last drew from it. In copied: offset in data.
};

struct Nx2UiSnapshotState {
    std::mutex mutex;
    /// Storage ranges UI draws read, noted by the GPU thread, copied at each present.
    std::vector<Nx2UiRange> ranges;
    /// The last present's copy, used while the GPU thread hasn't reached fence_value.
    std::atomic<bool> active{false};
    std::atomic<const Host1x::SyncpointManager*> syncpoints{nullptr};
    std::atomic<u32> fence_id{};
    std::atomic<u32> fence_value{};
    std::vector<Nx2UiRange> copied;
    std::vector<u8> data;
    /// [copied_begin, copied_end) spans every copied range: uploads outside it skip the mutex.
    std::atomic<u64> copied_begin{0};
    std::atomic<u64> copied_end{0};
    /// Ranges where an upload took the copy while guest memory already held the next frame's
    /// data. Marked CPU-written again once the copy is done with (marking them at the upload made
    /// every later bind in the frame upload them again: ~200 uploads of 140 KiB a present).
    std::vector<std::pair<u64, u64>> pending_remark;
    /// Those ranges, for the GPU thread's next draw (Nx2UiSnapshotTick).
    std::vector<std::pair<u64, u64>> remark_now;
    std::atomic<bool> remark_ready{false};
    /// For the log: uploads that took data from the copy, and how much.
    u64 patched_uploads{};
    u64 patched_bytes{};
    u64 remarked{};
    u64 compared_bytes{};

    /// Whether an upload of [address, address + size) may take data from the copy (lock-free).
    bool MayOverlap(u64 address, u64 size) const {
        return active.load(std::memory_order_acquire) &&
               address < copied_end.load(std::memory_order_relaxed) &&
               copied_begin.load(std::memory_order_relaxed) < address + size;
    }

    /// With mutex held: the copy is no longer used; its ranges written since are due.
    void RetireLocked() {
        active.store(false, std::memory_order_relaxed);
        if (!pending_remark.empty()) {
            remark_now.insert(remark_now.end(), pending_remark.begin(), pending_remark.end());
            pending_remark.clear();
            remark_ready.store(true, std::memory_order_release);
        }
    }

    /// Whether the GPU thread has passed the copy's present (lock-free).
    bool FrameDone() const {
        const auto* const manager = syncpoints.load(std::memory_order_acquire);
        return manager == nullptr ||
               manager->IsReadyGuest(fence_id.load(std::memory_order_relaxed),
                                     fence_value.load(std::memory_order_relaxed));
    }
};
inline Nx2UiSnapshotState g_nx2_ui_snapshot;
/// Presents so far, as the snapshot ranges count them.
inline std::atomic<u64> g_nx2_ui_snapshot_present{0};

inline constexpr u64 Nx2UiRangeMaxSize = 4ULL << 20;
inline constexpr size_t Nx2UiRangeMaxCount = 64;
inline constexpr u64 Nx2UiRangeAlignment = 4096;
/// A range not uploaded for this many presents is no longer copied.
inline constexpr u64 Nx2UiRangeMaxAge = 30;

/// GPU thread: a UI draw uploaded CPU-written data in [address, address + size) of a storage
/// buffer it only reads. Kept in whole pages, merged with the ranges it overlaps or touches.
inline void Nx2NoteUiStorage(u64 address, u64 size) {
    if (size == 0 || size > Nx2UiRangeMaxSize) {
        return;
    }
    const u64 begin = address & ~(Nx2UiRangeAlignment - 1);
    const u64 end = (address + size + Nx2UiRangeAlignment - 1) & ~(Nx2UiRangeAlignment - 1);
    const u64 present = g_nx2_ui_snapshot_present.load(std::memory_order_relaxed);
    auto& state = g_nx2_ui_snapshot;
    std::scoped_lock lock{state.mutex};
    for (auto& range : state.ranges) {
        const u64 range_end = range.address + range.size;
        if (begin <= range_end && range.address <= end) {
            const u64 merged_begin = (std::min)(begin, range.address);
            const u64 merged_end = (std::max)(end, range_end);
            if (merged_end - merged_begin <= Nx2UiRangeMaxSize) {
                range.address = merged_begin;
                range.size = merged_end - merged_begin;
                range.tag = present;
                return;
            }
        }
    }
    if (state.ranges.size() >= Nx2UiRangeMaxCount) {
        // Replace the one uploaded longest ago.
        auto oldest = std::ranges::min_element(state.ranges, {}, &Nx2UiRange::tag);
        *oldest = Nx2UiRange{begin, end - begin, present};
        return;
    }
    state.ranges.push_back(Nx2UiRange{begin, end - begin, present});
}

/// GPU thread, after reading [address, address + size) of guest memory into dst for an upload:
/// overwrites the parts the last present's copy holds while that present's frame is still being
/// processed. Where guest memory already differs from the copy (the game has written its next
/// frame), the range is marked CPU-written again once the copy retires, so the next frame's
/// draws upload it.
inline void Nx2PatchFromUiSnapshot(u64 address, u8* dst, u64 size) {
    auto& state = g_nx2_ui_snapshot;
    std::scoped_lock lock{state.mutex};
    if (!state.active.load(std::memory_order_relaxed)) {
        return;
    }
    if (state.FrameDone()) {
        state.RetireLocked();
        return;
    }
    bool patched = false;
    const u64 end = address + size;
    for (const Nx2UiRange& range : state.copied) {
        const u64 begin = (std::max)(address, range.address);
        const u64 stop = (std::min)(end, range.address + range.size);
        if (begin >= stop) {
            continue;
        }
        u8* const target = dst + (begin - address);
        const u8* const source = state.data.data() + range.tag + (begin - range.address);
        state.compared_bytes += stop - begin;
        if (std::memcmp(target, source, stop - begin) != 0) {
            std::memcpy(target, source, stop - begin);
            state.pending_remark.emplace_back(begin, stop - begin);
            state.patched_bytes += stop - begin;
            patched = true;
        }
    }
    if (patched) {
        ++state.patched_uploads;
    }
}

inline thread_local std::vector<u8> t_nx2_snapshot_scratch;

/// GPU thread: reads [address, address + size) of guest memory into the upload staging memory at
/// staging (read(address, pointer, size) reads guest memory), with the UI parts the last present
/// copied taken from the copy (Nx2PatchFromUiSnapshot). The staging memory may be mapped device
/// memory (VRAM through Resizable BAR on AMD) and is written once and never read: comparing in it
/// cost ~59 us a bind and froze the game for seconds on large uploads.
template <typename Read>
inline void Nx2ReadUploadWithUiSnapshot(u64 address, u8* staging, u64 size, Read&& read) {
    if (!g_nx2_ui_snapshot.MayOverlap(address, size)) {
        read(address, staging, size);
        return;
    }
    auto& scratch = t_nx2_snapshot_scratch;
    if (scratch.size() < size) {
        scratch.resize(size);
    }
    read(address, scratch.data(), size);
    Nx2PatchFromUiSnapshot(address, scratch.data(), size);
    std::memcpy(staging, scratch.data(), size);
}

/// GPU thread, before each draw binds its buffers: retires the copy once its present's frame is
/// done and marks the ranges the game wrote meanwhile CPU-written (remark(address, size)).
template <typename Remark>
inline void Nx2UiSnapshotTick(Remark&& remark) {
    auto& state = g_nx2_ui_snapshot;
    const bool due = state.remark_ready.load(std::memory_order_acquire);
    if (!due && !(state.active.load(std::memory_order_relaxed) && state.FrameDone())) {
        return;
    }
    std::scoped_lock lock{state.mutex};
    if (state.active.load(std::memory_order_relaxed) && state.FrameDone()) {
        state.RetireLocked();
    }
    for (const auto& [address, size] : state.remark_now) {
        remark(address, size);
    }
    state.remarked += state.remark_now.size();
    state.remark_now.clear();
    state.remark_ready.store(false, std::memory_order_relaxed);
}

} // namespace Tegra
