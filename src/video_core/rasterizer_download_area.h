// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "video_core/nx2.h"

namespace VideoCore {

/// The cache that reported a flush area (GetFlushArea), for statistics (GpuWaitStats).
enum class DownloadAreaSource : u8 {
    None,
    TextureCache,
    BufferCache,
};

struct RasterizerDownloadArea {
    VAddr start_address;
    VAddr end_address;
    bool preemtive;
    DownloadAreaSource source{};
    /// What guest memory lacks there is on its way for an earlier read: nothing to flush, only to
    /// wait for (RasterizerInterface::FinishRead).
    bool in_flight{};
};

/// Downloads for guest CPU reads that are submitted but not yet written to guest memory
/// (RasterizerInterface::FlushRegionForRead). While there are any, a write to guest memory from
/// the GPU's side first finishes those it overlaps (FinishRead), so none lands over it later.
inline std::atomic<u32> g_read_downloads{};

/// Whether such a download may still land over [addr, addr + size): what a write to guest memory
/// from the GPU's side asks before FinishRead, on any thread, without the buffer cache's lock.
/// The downloads take only GPU-modified bytes, and Switch 2 titles mark their pages
/// (Tegra::Nx2MayBeGpuWritten): writes to other pages skip the lock, which the GPU thread holds
/// for nearly every command.
[[nodiscard]] inline bool ReadDownloadsMayCover(DAddr addr, u64 size) {
    return g_read_downloads.load(std::memory_order_acquire) != 0 &&
           (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) ||
            Tegra::Nx2MayBeGpuWritten(addr, size));
}

/// TEST (EDEN_NX2_READ_CHECK): the latest writes from the GPU's side that ReadDownloadsMayCover
/// let through, to name the writer when the check finds newer bytes where an earlier read's
/// download landed.
class ReadCheckWrites {
public:
    [[nodiscard]] static bool Enabled() {
        static const bool enabled = std::getenv("EDEN_NX2_READ_CHECK") != nullptr;
        return enabled;
    }

    static void Note(DAddr addr, u64 size, const char* writer) {
        std::scoped_lock lock{mutex};
        ring[next++ % Size] = {addr, size, writer};
    }

    /// The newest noted write over [addr, addr + size).
    [[nodiscard]] static const char* Find(DAddr addr, u64 size) {
        std::scoped_lock lock{mutex};
        for (u64 i = 0; i < (std::min)(next, Size); ++i) {
            const Write& entry = ring[(next - 1 - i) % Size];
            if (entry.addr < addr + size && addr < entry.addr + entry.size) {
                return entry.writer;
            }
        }
        return "a writer the check doesn't hear of";
    }

private:
    struct Write {
        DAddr addr;
        u64 size;
        const char* writer;
    };
    static constexpr u64 Size = 1024;
    inline static std::mutex mutex;
    inline static std::array<Write, Size> ring{};
    inline static u64 next{};
};

} // namespace VideoCore
