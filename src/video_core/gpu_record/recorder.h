// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_record/record_format.h"

namespace Core {
class System;
}

namespace Service::Nvidia {
struct NvFence;
}

namespace Tegra {
struct CommandList;
struct FramebufferConfig;
class MemoryManager;
namespace Control {
struct ChannelState;
}
} // namespace Tegra

namespace VideoCommon::GpuRecord {

namespace Detail {
/// Set while a recording takes events (the hooks' only cost otherwise is this load).
inline std::atomic<bool> recording{false};
/// Set while a recording waits for the next guest frame boundary to start.
inline std::atomic<bool> armed{false};
/// Set from the start's capture until the end: guest CPU writes to GPU-visible pages are noted.
inline std::atomic<bool> tracking{false};
} // namespace Detail

[[nodiscard]] inline bool Recording() {
    return Detail::recording.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool Armed() {
    return Detail::armed.load(std::memory_order_relaxed);
}

[[nodiscard]] inline bool TrackingWrites() {
    return Detail::tracking.load(std::memory_order_relaxed);
}

struct RecordOptions {
    std::filesystem::path path;
    u32 frames = 60;
    /// Screenshot every Nth composite (0: none), at this size (the replay takes the same).
    u32 screenshot_every = 1;
    u32 screenshot_width = 1280;
    u32 screenshot_height = 720;
};

struct RecordStatus {
    std::string state; ///< idle, armed, recording, writing, done, failed
    std::string path;
    std::string error;
    u64 frames = 0;
    u64 submits = 0;
    u64 composites = 0;
    u64 dirty_pages = 0;
    u64 snapshot_pages = 0;
    u64 bytes = 0;
    double start_ms = 0.0;
    double write_ms = 0.0;
};

/// Records what the GPU emulation consumes for N guest frames (record_format.h): armed from the
/// debug server, it starts at the next guest frame boundary with a capture of the GPU's state
/// (GPU work drained, GPU-written data flushed to guest memory, engines, mappings, syncpoints,
/// every GPU-visible page), then notes command lists, presents, mappings, the guest's writes to
/// GPU-visible pages (they trap while recording) and the syncpoint values the guest had seen.
class Recorder {
public:
    static Recorder& Get();

    /// Starts at the next GuestFrameBoundary. False when a recording is armed or running.
    [[nodiscard]] bool Arm(Core::System& system, const RecordOptions& options, std::string& error);
    /// Not Status(): Xlib defines that name as a macro (Vulkan headers with X11 surfaces).
    [[nodiscard]] RecordStatus GetStatus() const;

    // Hooks. Each runs only while Recording() (OnFrameBoundary also while armed).
    void OnFrameBoundary();
    void OnSubmit(s32 channel, const Tegra::CommandList& entries);
    void OnComposite(const std::vector<Tegra::FramebufferConfig>& layers,
                     const std::vector<Service::Nvidia::NvFence>& fences, u64 request);
    void OnInitChannel(const Tegra::Control::ChannelState& channel);
    void OnSmmuMap(u64 address, u64 size);
    void OnSmmuUnmap(u64 address, u64 size);
    void OnGmmuOp(const Tegra::MemoryManager& memory_manager, GmmuOp op);
    void OnSyncpointIncrement(u32 id, bool guest, bool host);
    /// A GPU-ordered operation (GPU RunInOrder/MapInOrder): wraps it so the GPU page table
    /// changes it makes land at its place in the stream.
    [[nodiscard]] std::function<void()> WrapOrdered(std::function<void()>&& func,
                                                    bool before_later_reads);
    /// A guest CPU write trapped on a GPU-visible page (Core::Memory, while TrackingWrites()).
    void OnCpuWrite(const u8* host_pointer, size_t size);
    /// The renderer is about to show composite `request` (OnComposite's; GPU thread).
    void OnCompositeExecuted(u64 request);

    /// Whether whoever calls IncrementGuest/Host is the GPU's own fence path (not recorded).
    static inline thread_local bool gpu_increment = false;

    struct Impl;

private:
    Recorder();
    ~Recorder();
    std::unique_ptr<Impl> impl;
};

/// Marks the calling thread's syncpoint increments as the GPU's own while alive.
struct GpuIncrementScope {
    GpuIncrementScope() : previous{Recorder::gpu_increment} {
        Recorder::gpu_increment = true;
    }
    ~GpuIncrementScope() {
        Recorder::gpu_increment = previous;
    }
    bool previous;
};

/// Converts BGRA screenshot pixels (bottom-up when invert_y) to a PNG.
[[nodiscard]] std::vector<u8> EncodeScreenshotPng(std::span<const u8> bgra, u32 width, u32 height,
                                                  bool invert_y);

} // namespace VideoCommon::GpuRecord
