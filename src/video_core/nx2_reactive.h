// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"

// Reactive flushing of GPU-written buffer data on Switch 2 titles. Their GPU writes are not
// copied back at fences (BufferCache::AsyncBufferDownloads): a guest CPU read of a GPU-written
// page traps, drains the GPU thread and waits for the host GPU to go idle (Scheduler::Finish)
// before the data is copied back (GPU::OnCPURead). Ocarina takes ~160 of these a second, ~5 a
// frame, mostly on a few hundred pages it reads every frame.
namespace VideoCommon::Nx2Reactive {

// GPU writes over pages the guest CPU has read GPU data from (BufferCache::cpu_read_ranges) are
// also copied back at the next fence (prefetch); their traps stay armed. A read after the copy
// reaches guest memory goes on without a stall and sees current data. An earlier one, whose
// GPU writes all have a download recorded, waits for those downloads to land instead of
// flushing (GPU::WaitForLanding, at most 16 ms). A read with no download on its way has the GPU
// thread queue the data for download at a fence it submits at once
// (RasterizerInterface::QueueFlushRegion) and waits for it to land the same way, instead of the
// GPU thread copying it and waiting for the host GPU to go idle (Scheduler::Finish); image data
// still flushes. A pop skips the guest CPU writes not yet applied
// (BufferCache::PendingCpuWriteRanges), so it cannot write the GPU's older bytes over them.

/// Pages read keep their later GPU writes copied back while they are read at least once every
/// HotFrames to 2 * HotFrames frames. A forgotten page's next read flushes as a first read does.
inline constexpr u64 HotFrames = 64;

/// Frames ended so far (FenceManager::TickFrame).
inline std::atomic<u64> g_frame{};

/// Wakes the reads waiting for downloads to land (WaitLanding) when a fence's downloads pop.
struct Landing {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<u64> generation{};
    std::atomic<u32> waiters{};
};

inline Landing& GetLanding() {
    static Landing landing;
    return landing;
}

inline void NotifyLanded() {
    Landing& landing = GetLanding();
    landing.generation.fetch_add(1, std::memory_order_acq_rel);
    if (landing.waiters.load(std::memory_order_acquire) != 0) {
        std::scoped_lock lock{landing.mutex};
        landing.cv.notify_all();
    }
}

} // namespace VideoCommon::Nx2Reactive
