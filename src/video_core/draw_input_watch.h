// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"

// Tells the GPU thread, without reading guest memory, that the memory a draw read is unchanged.
//
// A draw reads guest memory the registers don't cover: texture handles and storage buffer
// addresses in constant buffers, and texture pool entries. Guest CPU writes to a page are only
// seen while the page is write-protected (rasterizer cached), and the buffer cache drops that
// protection for pages it holds CPU-modified. So the watch holds its own protection on each page
// a draw read (an extra cached count) and arms the page's bucket in common/input_watch.h: a guest
// write then moves the bucket's epoch. The GPU emulation's own writes (constant buffer data,
// inline uploads, DMA) are host writes that don't trap: they arrive through the rasterizer's
// invalidations (NoteHostWrite) and move a version per 64-byte granule.
//
// A snapshot of a draw's inputs taken while every page was armed, and with no guest write to
// an armed bucket while the draw read them, stands while no epoch or version it holds moves.
// Pages the GPU writes (render targets, storage, queries, downloads) are never trusted: their
// bytes change without either signal.
namespace VideoCommon {

class DrawInputWatch {
public:
    /// What a page holds for the draw (statistics).
    enum class Use : u8 { Handles, Pools, Storage, Uniform, Count };

    struct Range {
        DAddr address;
        u32 size;
        Use use;
    };

    struct PageInput {
        u64 page;
        u32 bucket;
        u32 epoch;
    };
    struct GranuleInput {
        u32 slot;
        u32 version;
    };
    struct Snapshot {
        u64 host_epoch{};
        boost::container::small_vector<PageInput, 8> pages;
        boost::container::small_vector<GranuleInput, 24> granules;
    };

    /// delta +1 protects [address, address + size), -1 releases it (UpdatePagesCachedCount).
    using ProtectFn = void (*)(void* context, DAddr address, u64 size, s32 delta);
    /// Whether the GPU may write the page: textures, queries or GPU-written buffers live there.
    using GpuPageFn = bool (*)(void* context, DAddr page_address);

    static constexpr u32 GranuleBits = 6;
    static constexpr size_t NumGranuleSlots = size_t{1} << 16;
    static constexpr u64 MaxGranuleWrite = 64 * 1024; ///< larger host writes move host_epoch

    DrawInputWatch();
    ~DrawInputWatch();

    void Bind(ProtectFn protect, GpuPageFn gpu_page, void* context);
    /// Releases every page and forgets the callbacks (the rasterizer goes away).
    void Unbind();

    /// The GPU emulation wrote [address, address + size). Any thread.
    void NoteHostWrite(DAddr address, u64 size);

    /// [address, address + size) is being unmapped: releases its pages. Any thread.
    void ReleaseRange(DAddr address, u64 size);

    /// Guest writes to armed buckets so far: a draw whose reads span a change is not captured.
    [[nodiscard]] u64 GuestWrites() const;

    /// Arms the pages of `ranges` and, when all of them were armed before this call and stay
    /// trusted, fills `out` and returns true. `guest_writes_before` is GuestWrites() taken before
    /// the draw read the ranges. GPU thread.
    bool Capture(std::span<const Range> ranges, u64 frame, u64 guest_writes_before, Snapshot& out);

    /// Arms the pages of `ranges` for the statistics only (EDEN_NX2_DRAW_WATCH_UBO).
    void ArmOnly(std::span<const Range> ranges, u64 frame);

    /// Whether nothing `snapshot` covers was written since it was captured. GPU thread.
    [[nodiscard]] bool Unchanged(const Snapshot& snapshot) const;

    /// Releases the pages a guest write broke, once a frame. GPU thread.
    void Sweep(u64 frame);

    /// EDEN_NX2_DRAW_WATCH_UBO=1: also arm the uniform buffers' pages, to count their writes.
    [[nodiscard]] static bool WatchUniforms();

    /// The frame Sweep was last called for.
    [[nodiscard]] u64 Frame() const {
        return current_frame;
    }

    /// Logs the watch statistics of the last `seconds` (EDEN_NX2_DRAW_CLASSES).
    void Report(double seconds);

    /// Statistics for the watch's log line.
    struct Stats {
        std::array<u64, static_cast<size_t>(Use::Count)> armed_pages{};
        std::array<u64, static_cast<size_t>(Use::Count)> breaks{};
        u64 arms{};
        u64 releases{};
        u64 untrusted{};
        u64 guest_writes{};
        u64 handler_ns{};
    };
    [[nodiscard]] Stats TakeStats();

    /// The pure decisions, kept apart for the tests.
    [[nodiscard]] static u32 GranuleSlot(u64 granule);
    /// Frames a page waits before it is armed again after its `breaks`-th guest write.
    [[nodiscard]] static u64 RearmDelay(u32 breaks);

private:
    struct Page {
        u64 rearm_frame{};
        u32 armed_epoch{};
        u32 breaks{};
        u8 uses{}; ///< Use bits it was armed for
        bool armed{};
    };

    /// Arms `page` for `use`. Returns 1 when it was armed already, 0 when armed now, -1 when it
    /// can't be armed (GPU-written, or waiting after a guest write).
    int Arm(u64 page, Use use, u64 frame);
    void Release(u64 page, Page& state, bool broken);

    ProtectFn protect{};
    GpuPageFn gpu_page{};
    void* context{};

    std::unique_ptr<std::atomic<u32>[]> granule_versions;
    std::atomic<u64> host_epoch{1};

    mutable std::mutex mutex;
    std::unordered_map<u64, Page> pages;
    Stats stats{};
    u64 current_frame{};
    u64 last_guest_writes{};
    u64 last_handler_ns{};
};

[[nodiscard]] DrawInputWatch& GetDrawInputWatch();

} // namespace VideoCommon
