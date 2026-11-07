// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdlib>

#include "common/assert.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/nx2_profile.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

namespace {

constexpr u32 Gf100BindClassMask = 0xffff;
constexpr u32 Gf100BindValidMask =  0x1f0000 | Gf100BindClassMask;

// Reads a semaphore word, flushing a GPU-written copy out of the buffer cache first.
u32 ReadSemaphoreWord(DmaPusher& dma_pusher, GPUVAddr address) {
    if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(address);
        device && dma_pusher.rasterizer->MustFlushRegion(*device, sizeof(u32),
                                                          VideoCommon::CacheType::BufferCache)) {
        VideoCommon::GpuSplit::Get().Run(
            [rasterizer = dma_pusher.rasterizer, device = *device] {
                rasterizer->FlushRegion(device, sizeof(u32), VideoCommon::CacheType::BufferCache);
            },
            VideoCommon::DrainSite::SemaphoreFlush);
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// With the GPU thread split (gpu_split.h) the rasterizer belongs to the back thread: work that
// only feeds it runs there in order; reads of what it wrote first wait for it (DrainBack).
template <typename Func>
void OnBack(const Func& func) {
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(func);
    } else {
        func();
    }
}

void DrainBack() {
    VideoCommon::GpuSplit::Get().Drain(VideoCommon::DrainSite::Acquire);
}

// A semaphore release just queued for the back: the payload, and the time stamp unless it is a
// one-word release (IsAFence without HasTimeout), for the narrow drains.
void NoteRelease(DmaPusher& dma_pusher, GPUVAddr address, u32 payload, bool one_word) {
    auto& split = VideoCommon::GpuSplit::Get();
    split.NoteWrite(&dma_pusher.memory_manager, address, one_word ? 4 : 16, true);
    split.NoteRelease(&dma_pusher.memory_manager, address, payload);
}

// EDEN_NX2_SEM_PREDICT: met by a release still queued for the back (GpuSplit::NoteRelease). The
// work after the acquire is queued behind the release, as the hardware would run it once the
// release lands, and command list reads wait for the work before the release (NoteSyncAt).
// `matches` checks a release's value; `at_least` a max release's (EDEN_NX2_QMD_PREDICT), after
// which the word holds at least that value.
template <typename Matches, typename AtLeast>
bool PredictedMet(DmaPusher& dma_pusher, GPUVAddr address, const Matches& matches,
                  const AtLeast& at_least) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active() || !split.SemaphorePredict() || VideoCommon::GpuSplit::NarrowVerify()) {
        return false;
    }
    const std::optional<VideoCommon::SemaphorePrediction> predicted =
        split.PredictedRelease(&dma_pusher.memory_manager, address);
    if (!predicted ||
        !(predicted->at_least ? at_least(predicted->value) : matches(predicted->value))) {
        return false;
    }
    split.NoteSyncAt(predicted->position, VideoCommon::UnboundedCause::PredictedSync);
    split.NoteAcquireDiag(true, false);
    return true;
}

// Narrow drains (GpuSplit::NarrowDrains): the semaphore word once the queued work that may write
// it ran, not all of it. Nothing where only a full drain tells (GPU-written pages, where the
// buffer cache may hold a newer word): the caller drains and reads it with ReadSemaphoreWord.
std::optional<u32> NarrowSemaphoreRead(DmaPusher& dma_pusher, GPUVAddr address) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active() || !split.NarrowDrains()) {
        return std::nullopt;
    }
    // Acquire base: QMD releases are written by dispatches (EDEN_NX2_NARROW_COMPUTE keeps those
    // out of the read position). With EDEN_NX2_QMD_PREDICT the front records them as writes
    // instead, and launches only count where a grid ran without EDEN_NX2_ACQUIRE_POLL.
    const bool waited = split.WaitForWritesTo(&dma_pusher.memory_manager, address, sizeof(u32),
                                              VideoCommon::DrainSite::NarrowAcquire,
                                              VideoCommon::NarrowBase::Acquire);
    split.NoteAcquireDiag(false, waited);
    if (dma_pusher.memory_manager.Nx2ReadNeedsDrain(address, sizeof(u32))) {
        return std::nullopt;
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// EDEN_NX2_ACQUIRE_POLL: the semaphore word from guest memory once the recorded writes over it
// ran. A hardware acquire doesn't wait for earlier dispatches either, and a word that doesn't meet
// the acquire yet is polled again (CanResumeAcquire) until the CPU or the back writes it, so no
// other queued work is waited for. Nothing where the buffer cache may hold a newer word
// (GPU-written pages): the caller drains and reads it with ReadSemaphoreWord.
std::optional<u32> PollSemaphoreRead(DmaPusher& dma_pusher, GPUVAddr address) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (!split.Active() || !split.AcquirePoll() || VideoCommon::GpuSplit::NarrowVerify()) {
        return std::nullopt;
    }
    const bool waited = split.WaitForWritesTo(&dma_pusher.memory_manager, address, sizeof(u32),
                                              VideoCommon::DrainSite::NarrowAcquire,
                                              VideoCommon::NarrowBase::Recorded);
    split.NoteAcquireDiag(false, waited);
    if (dma_pusher.memory_manager.Nx2ReadNeedsDrain(address, sizeof(u32))) {
        return std::nullopt;
    }
    return dma_pusher.memory_manager.Read<u32>(address);
}

// A met acquire on a word the GPU releases orders the command list reads after it behind the
// work before the release (NoteSyncDone: it ran, the word is in guest memory); one only the CPU
// writes doesn't (GpuSplit::AcquireSyncs).
void NoteAcquireSync(GPUVAddr address) {
    auto& split = VideoCommon::GpuSplit::Get();
    if (split.AcquireSyncs(address)) {
        split.NoteSyncDone(VideoCommon::UnboundedCause::AcquireSync);
    }
}

// The fence manager records into the scheduler and downloads the CPU's writes over GPU data:
// the back's state, which a drained back still uses from its idle hook (it submits).
void ReleaseFences(DmaPusher& dma_pusher) {
    VideoCommon::GpuSplit::Get().Run(
        [rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); },
        VideoCommon::DrainSite::FenceRelease);
}

// EDEN_NX2_FRONT_DEFER: an acquire whose check would wait for the back parks the channel at
// the acquire instead (Puller::StepDeferred). It mirrors the polled check, so it needs it.
bool DeferAcquires() {
    auto& split = VideoCommon::GpuSplit::Get();
    return split.Active() && split.FrontDefer() && split.AcquirePoll() &&
           !VideoCommon::GpuSplit::NarrowVerify();
}

} // Anonymous namespace

Puller::DeferResult Puller::StepDeferred(DmaPusher& dma_pusher) {
    auto& split = VideoCommon::GpuSplit::Get();
    auto& memory = dma_pusher.memory_manager;
    auto& waiters = Nx2SemaphoreWaiters::Get();
    PendingAcquire& acquire = *pending_acquire;
    // Met by a release sent or landed since (another channel's, sent while this one was
    // parked), as a suspended acquire is.
    if (PredictedMet(
            dma_pusher, acquire.address, [&acquire](u32 value) { return acquire.Matches(value); },
            [&acquire](u32 lower) {
                return acquire.operation ==
                           static_cast<u32>(GpuSemaphoreOperation::AcquireGequal) &&
                       lower >= acquire.value;
            })) {
        return DeferResult::Met;
    }
    if (acquire.watched && waiters.Latched(this)) {
        return DeferResult::Met;
    }
    // The split stopped (or started again) meanwhile: its positions mean nothing any more, and a
    // drain stands in for each of them.
    const bool split_gone = !split.Active() || split.Session() != acquire.session;
    const auto read = [&](bool needs_drain) {
        if (needs_drain) {
            DrainBack();
            return ReadSemaphoreWord(dma_pusher, acquire.address);
        }
        return memory.Read<u32>(acquire.address);
    };
    const auto watch = [&] {
        // From here other channels' work may run before the check ends: a GPU release among it
        // that meets the value counts even if a later one overwrites it.
        if (!acquire.watched) {
            if (const auto device = memory.GpuToCpuAddress(acquire.address)) {
                waiters.Add(this, *device, acquire.value, acquire.operation);
            }
            acquire.watched = true;
        }
    };
    for (;;) {
        u64 target{};
        VideoCommon::DeferKind kind{};
        switch (acquire.stage) {
        case DeferStage::Poll:
            target = acquire.poll_target;
            kind = VideoCommon::DeferKind::AcquirePoll;
            break;
        case DeferStage::Narrow:
            target = acquire.narrow_target;
            kind = VideoCommon::DeferKind::AcquireNarrow;
            break;
        case DeferStage::Full:
            target = acquire.sent;
            kind = VideoCommon::DeferKind::AcquireFull;
            break;
        case DeferStage::Fences:
            target = acquire.fence_target;
            kind = VideoCommon::DeferKind::AcquireFences;
            break;
        default:
            return DeferResult::Suspended;
        }
        if (!split_gone && !split.Reached(target)) {
            watch();
            split.NoteDeferredWait(target);
            if (acquire.parked != acquire.stage) {
                acquire.parked = acquire.stage;
                split.NoteDeferDiag(kind);
            }
            return DeferResult::Waiting;
        }
        // Where only a drain tells (GPU-written pages: the buffer cache may hold a newer word),
        // the stages read as the blocking check does, after a drain.
        const bool needs_drain =
            split_gone || memory.Nx2ReadNeedsDrain(acquire.address, sizeof(u32));
        switch (acquire.stage) {
        case DeferStage::Poll:
            // PollSemaphoreRead: met once the recorded writes over the word ran.
            if (!needs_drain && acquire.Matches(memory.Read<u32>(acquire.address))) {
                NoteAcquireSync(acquire.address);
                return DeferResult::Met;
            }
            acquire.stage = DeferStage::Narrow;
            break;
        case DeferStage::Narrow:
            // NarrowSemaphoreRead: the release may be a QMD's, behind the dispatches.
            if (!needs_drain && acquire.Matches(memory.Read<u32>(acquire.address))) {
                split.NoteSyncDone(VideoCommon::UnboundedCause::AcquireSync);
                return DeferResult::Met;
            }
            acquire.stage = DeferStage::Full;
            break;
        case DeferStage::Full: {
            // The drained read: everything sent before the acquire ran.
            if (acquire.Matches(read(needs_drain))) {
                return DeferResult::Met;
            }
            // Registered before the fences run, as the blocking check.
            watch();
            // ReleaseFences runs on the back in order: its position, not a drain.
            OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
            acquire.fence_target = split.Active() ? split.WritePosition() : 0;
            acquire.stage = DeferStage::Fences;
            break;
        }
        case DeferStage::Fences: {
            const u32 current = read(needs_drain);
            if (waiters.Latched(this) || acquire.Matches(current)) {
                return DeferResult::Met;
            }
            acquire.checked = acquire.fence_target;
            return DeferResult::Suspended;
        }
        default:
            return DeferResult::Suspended;
        }
    }
}

void Puller::BeginSuspension(DmaPusher& dma_pusher) {
    pending_acquire->stage = DeferStage::Suspended;
    dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
    VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
}

bool Puller::CanResumeSuspended(DmaPusher& dma_pusher) {
    auto& split = VideoCommon::GpuSplit::Get();
    auto& memory = dma_pusher.memory_manager;
    auto& waiters = Nx2SemaphoreWaiters::Get();
    PendingAcquire& acquire = *pending_acquire;
    if (acquire.session != split.Session()) {
        // Suspended before the split (re)started: check against all of its work.
        acquire.session = split.Session();
        acquire.checked = 0;
    }
    // As the polled read: the recorded writes over the word ran. CPU releases show here.
    const u64 recorded = split.WritesToTarget(&memory, acquire.address, sizeof(u32),
                                              VideoCommon::NarrowBase::Recorded);
    if (split.Reached(recorded)) {
        if (acquire.Matches(memory.Read<u32>(acquire.address)) || waiters.Latched(this)) {
            NoteAcquireSync(acquire.address);
            return true;
        }
    } else {
        split.NoteDeferredWait(recorded);
    }
    // A release the front doesn't record (a QMD's) may be queued in the work sent since the
    // last full check. The blocking poll drained for it each time; this reads once the back got
    // there and has the GPU thread wait for that only when it has nothing else to run.
    const u64 sent = split.WritePosition();
    if (sent > acquire.checked) {
        const u64 narrow = split.WritesToTarget(&memory, acquire.address, sizeof(u32),
                                                VideoCommon::NarrowBase::Acquire);
        const bool narrow_reached = split.Reached(narrow);
        if (narrow_reached && narrow > acquire.checked &&
            acquire.Matches(memory.Read<u32>(acquire.address))) {
            split.NoteSyncDone(VideoCommon::UnboundedCause::AcquireSync);
            return true;
        }
        if (split.Reached(sent)) {
            acquire.checked = sent;
            if (acquire.Matches(memory.Read<u32>(acquire.address))) {
                return true;
            }
        } else {
            split.NoteDeferredWait(narrow_reached ? sent : narrow);
            if (acquire.fence_target != sent) {
                acquire.fence_target = sent;
                split.NoteDeferDiag(VideoCommon::DeferKind::Suspended);
            }
        }
    }
    return waiters.Latched(this);
}

bool Puller::PendingAcquire::Matches(u32 current) const {
    switch (static_cast<GpuSemaphoreOperation>(operation)) {
    case GpuSemaphoreOperation::AcquireEqual:
        return current == value;
    case GpuSemaphoreOperation::AcquireGequal:
        return current >= value;
    case GpuSemaphoreOperation::AcquireMask:
        return (current & value) != 0;
    default:
        return true;
    }
}

bool Puller::CanResumeAcquire(DmaPusher& dma_pusher) {
    if (!pending_acquire) {
        return true;
    }
    if (pending_acquire->met) {
        return true;
    }
    if (pending_acquire->stage != DeferStage::Suspended) {
        // EDEN_NX2_FRONT_DEFER: a parked check goes on where it stopped.
        switch (StepDeferred(dma_pusher)) {
        case DeferResult::Met:
            pending_acquire->met = true;
            return true;
        case DeferResult::Suspended:
            BeginSuspension(dma_pusher);
            return false;
        case DeferResult::Waiting:
            return false;
        }
        return false;
    }
    if (PredictedMet(
            dma_pusher, pending_acquire->address,
            [this](u32 value) { return pending_acquire->Matches(value); },
            [this](u32 lower) {
                return pending_acquire->operation ==
                           static_cast<u32>(GpuSemaphoreOperation::AcquireGequal) &&
                       lower >= pending_acquire->value;
            })) {
        return true;
    }
    // Once, after 2 s without the value, the drained read below (it reports the wait).
    const bool overdue =
        !pending_acquire->reported &&
        std::chrono::steady_clock::now() - pending_acquire->since >= std::chrono::seconds{2};
    if (!overdue && DeferAcquires() &&
        !dma_pusher.memory_manager.Nx2ReadNeedsDrain(pending_acquire->address, sizeof(u32))) {
        // EDEN_NX2_FRONT_DEFER: no drain per poll (below); the overdue report still drains.
        if (CanResumeSuspended(dma_pusher)) {
            pending_acquire->met = true;
            return true;
        }
        return false;
    }
    const std::optional<u32> polled =
        overdue ? std::optional<u32>{} : PollSemaphoreRead(dma_pusher, pending_acquire->address);
    if (polled &&
        (pending_acquire->Matches(*polled) || Nx2SemaphoreWaiters::Get().Latched(this))) {
        NoteAcquireSync(pending_acquire->address);
        return true;
    }
    // Not met in guest memory: the release may still be queued for the back (one the front
    // doesn't record, such as a QMD release or a report). Check it as below rather than wait for
    // the next poll: suspended channels are polled with yields and 1 ms sleeps, and that latency
    // (hundreds of acquires a second) took OoT from 39 to 20 FPS. With the back caught up the
    // drain returns at once, so an acquire the CPU releases costs no wait on the back.
    // Met without waiting for all queued work: the work after it is queued behind the work
    // before it, and command list reads wait for that work (NoteSync).
    std::optional<u32> narrowed = NarrowSemaphoreRead(dma_pusher, pending_acquire->address);
    if (narrowed && pending_acquire->Matches(*narrowed)) {
        if (!VideoCommon::GpuSplit::NarrowVerify()) {
            VideoCommon::GpuSplit::Get().NoteSyncDone(VideoCommon::UnboundedCause::AcquireSync);
            return true;
        }
    } else {
        narrowed.reset();
    }
    // The release may still be queued for the split GPU thread's back half (and the caches
    // ReadSemaphoreWord asks are its state).
    DrainBack();
    const u32 current = ReadSemaphoreWord(dma_pusher, pending_acquire->address);
    if (narrowed) {
        VideoCommon::GpuSplit::ReportNarrowCheck(VideoCommon::DrainSite::NarrowAcquire,
                                                 *narrowed != current ? 1U : 0U, 0,
                                                 pending_acquire->address);
    }
    if (Nx2SemaphoreWaiters::Get().Latched(this)) {
        if (static std::atomic<u32> rescued{};
            !pending_acquire->Matches(current) && rescued.fetch_add(1) < 64) {
            LOG_INFO(HW_GPU, "Channel {} met semaphore {:#x} value {:#x} at its release (now {:#x})",
                     dma_pusher.channel_state.bind_id, pending_acquire->address,
                     pending_acquire->value, current);
        }
        return true;
    }
    if (pending_acquire->Matches(current)) {
        return true;
    }
    if (!pending_acquire->reported &&
        std::chrono::steady_clock::now() - pending_acquire->since > std::chrono::seconds{2}) {
        pending_acquire->reported = true;
        const auto device = dma_pusher.memory_manager.GpuToCpuAddress(pending_acquire->address);
        LOG_WARNING(HW_GPU, "Channel {} waits on semaphore {:#x} (op {}, {:#x}, now {:#x}) for 2 s; "
                    "device {:#x}, buffer cache holds GPU data: {}, words {:08x} {:08x} {:08x} {:08x}",
                    dma_pusher.channel_state.bind_id, pending_acquire->address,
                    pending_acquire->operation, pending_acquire->value, current, device.value_or(0),
                    device && dma_pusher.rasterizer->MustFlushRegion(*device, 16, VideoCommon::CacheType::All),
                    dma_pusher.memory_manager.Read<u32>(pending_acquire->address & ~GPUVAddr{0xf}),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 4),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 8),
                    dma_pusher.memory_manager.Read<u32>((pending_acquire->address & ~GPUVAddr{0xf}) + 12));
    }
    return false;
}

bool Puller::ResumeAcquire(DmaPusher& dma_pusher) {
    if (!CanResumeAcquire(dma_pusher)) {
        return false;
    }
    // Only a resume from a suspension counts: DmaPusher::Step calls this before every entry, and
    // counting those made the count odd ("suspended") at random, so submit-time snapshots were
    // thrown away for about half of all entries (CommandList::snapshot_suspensions).
    if (pending_acquire) {
        // A check that only parked (EDEN_NX2_FRONT_DEFER) never counted as a suspension.
        const bool suspended = pending_acquire->stage == DeferStage::Suspended;
        if (!suspended) {
            VideoCommon::GpuSplit::Get().NoteDeferResume(pending_acquire->sent);
        }
        pending_acquire.reset();
        if (suspended) {
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
        }
        Nx2SemaphoreWaiters::Get().Remove(this);
    }
    return true;
}

void Puller::ProcessBindMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_DEBUG(HW_GPU, "Binding subchannel {} to engine {:#x}", method_call.subchannel, method_call.argument);
    u32 engine = method_call.argument;
    if ((engine & ~Gf100BindClassMask) != 0 && (engine & ~Gf100BindValidMask) == 0) {
        engine &= Gf100BindClassMask;
    }
    // Switch 2 binds Ampere classes; run them on the Maxwell engines (same method layout for
    // most state). Class 0 unbinds the subchannel.
    switch (engine & 0xFFFF) {
    case 0xC797: // AMPERE_B
        engine = static_cast<u32>(EngineID::MAXWELL_B);
        break;
    case 0xC7C0: // AMPERE_COMPUTE_B
        engine = static_cast<u32>(EngineID::KEPLER_COMPUTE_B);
        break;
    case 0xC7B5: // AMPERE_DMA_COPY_B
        engine = static_cast<u32>(EngineID::MAXWELL_DMA_COPY_A);
        break;
    case 0x0:
        if (g_nx2_ampere) {
            LOG_DEBUG(HW_GPU, "Switch 2 unbinds subchannel {}", method_call.subchannel);
            return;
        }
        break;
    default:
        break;
    }

    const auto engine_id = static_cast<EngineID>(engine);
    bound_engines[method_call.subchannel] = engine_id;
    switch (engine_id) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->fermi_2d, method_call.subchannel, EngineTypes::Fermi2D);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_3d, method_call.subchannel, EngineTypes::Maxwell3D);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_compute, method_call.subchannel, EngineTypes::KeplerCompute);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->maxwell_dma, method_call.subchannel, EngineTypes::MaxwellDMA);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->kepler_memory, method_call.subchannel, EngineTypes::KeplerMemory);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.BindSubchannel(&dma_pusher.channel_state.payload->nv01_timer, method_call.subchannel, EngineTypes::Nv01Timer);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine {:04X}", engine_id);
        break;
    }
}

void Puller::ProcessFenceActionMethod(DmaPusher& dma_pusher) {
    switch (regs.fence_action.op) {
    case Puller::FenceOperation::Acquire:
        // UNIMPLEMENTED_MSG("Channel Scheduling pending.");
        // WaitFence(regs.fence_action.syncpoint_id, regs.fence_value);
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->ReleaseFences(); });
        VideoCommon::GpuSplit::Get().NoteSync(VideoCommon::UnboundedCause::SyncpointAcquire);
        // A forced release writes what the fences before it hold.
        VideoCommon::GpuSplit::Get().NoteUnboundedWrite(
            false, VideoCommon::UnboundedCause::SyncpointAcquire);
        break;
    case Puller::FenceOperation::Increment:
        OnBack([rasterizer = dma_pusher.rasterizer, id = static_cast<u32>(regs.fence_action.syncpoint_id.Value())] {
            rasterizer->SignalSyncPoint(id);
        });
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented operation {}", regs.fence_action.op.Value());
        break;
    }
}

void Puller::ProcessSemaphoreTriggerMethod(DmaPusher& dma_pusher) {
    const auto semaphoreOperationMask = 0xF;
    const auto op = GpuSemaphoreOperation(regs.semaphore_trigger & semaphoreOperationMask);
    // EDEN_NX2_SEM_WATCH=<gpu address>: log every release/acquire on the 16 bytes there.
    static const GPUVAddr watch = [] {
        const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
        return value ? GPUVAddr{std::strtoull(value, nullptr, 16)} : GPUVAddr{};
    }();
    if (watch != 0 && (regs.semaphore_address.SemaphoreAddress() & ~GPUVAddr{0xf}) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} as {} device {:#x} op {} {:#x} value {:#x} (now {:#x})",
                 dma_pusher.channel_state.bind_id, dma_pusher.memory_manager.GetID(),
                 dma_pusher.memory_manager.GpuToCpuAddress(regs.semaphore_address.SemaphoreAddress()).value_or(0),
                 static_cast<u32>(op),
                 regs.semaphore_address.SemaphoreAddress(), regs.semaphore_sequence,
                 dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress()));
    }
    if (op == GpuSemaphoreOperation::WriteLong) {
        const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
        const u32 payload = regs.semaphore_sequence;
        // RELEASE_SIZE (bit 24) set means a 4-byte release without the timestamp; writing 16
        // bytes there clobbers the words next to it (Switch 2 packs semaphores tightly).
        const auto properties = g_nx2_ampere && (regs.semaphore_trigger & (1U << 24)) != 0
                                    ? VideoCommon::QueryPropertiesFlags::IsAFence
                                    : VideoCommon::QueryPropertiesFlags::HasTimeout;
        OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, properties, payload] {
            rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload, properties, payload, 0);
        });
        NoteRelease(dma_pusher, sequence_address, payload,
                    properties == VideoCommon::QueryPropertiesFlags::IsAFence);
    } else if (g_nx2_ampere) {
        // Switch 2 waits on semaphores the CPU (or another channel) releases, e.g. once it has
        // written the commands this channel runs next: suspend the channel until the value is
        // met instead of reading ahead (approach from changes.patch).
        PendingAcquire acquire{
            .address = regs.semaphore_address.SemaphoreAddress(),
            .value = regs.semaphore_sequence,
            .operation = static_cast<u32>(op),
            .since = std::chrono::steady_clock::now(),
            .reported = false,
        };
        if (op != GpuSemaphoreOperation::AcquireEqual && op != GpuSemaphoreOperation::AcquireGequal &&
            op != GpuSemaphoreOperation::AcquireMask) {
            LOG_ERROR(HW_GPU, "Invalid semaphore operation {}", static_cast<u32>(op));
            return;
        }
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireChecks, 1);
        // Already met: as in CanResumeAcquire.
        if (PredictedMet(
                dma_pusher, acquire.address, [&acquire](u32 value) { return acquire.Matches(value); },
                [&acquire](u32 lower) {
                    return acquire.operation ==
                               static_cast<u32>(GpuSemaphoreOperation::AcquireGequal) &&
                           lower >= acquire.value;
                })) {
            return;
        }
        if (DeferAcquires()) {
            // EDEN_NX2_FRONT_DEFER: the checks below, each once the back reached its position
            // (taken now, as the blocking check takes them); until then the channel parks here
            // and the front runs the other channels' work.
            auto& split = VideoCommon::GpuSplit::Get();
            acquire.session = split.Session();
            acquire.poll_target =
                split.WritesToTarget(&dma_pusher.memory_manager, acquire.address, sizeof(u32),
                                     VideoCommon::NarrowBase::Recorded);
            acquire.narrow_target =
                split.WritesToTarget(&dma_pusher.memory_manager, acquire.address, sizeof(u32),
                                     VideoCommon::NarrowBase::Acquire);
            acquire.sent = split.WritePosition();
            acquire.stage = DeferStage::Poll;
            pending_acquire = acquire;
            const DeferResult result = StepDeferred(dma_pusher);
            split.NoteAcquireDiag(false, result != DeferResult::Met);
            if (result == DeferResult::Met) {
                if (pending_acquire->watched) {
                    Nx2SemaphoreWaiters::Get().Remove(this);
                }
                pending_acquire.reset();
            } else if (result == DeferResult::Suspended) {
                BeginSuspension(dma_pusher);
            }
            return;
        }
        if (const std::optional<u32> polled = PollSemaphoreRead(dma_pusher, acquire.address);
            polled && acquire.Matches(*polled)) {
            NoteAcquireSync(acquire.address);
            return;
        }
        // Not met in guest memory: the release may still be queued for the back, so check it
        // with the drained read below before suspending (see CanResumeAcquire). Suspending at
        // once left the channel to the pending-channel polling, whose latency halved the FPS.
        std::optional<u32> narrowed = NarrowSemaphoreRead(dma_pusher, acquire.address);
        if (narrowed && acquire.Matches(*narrowed)) {
            if (!VideoCommon::GpuSplit::NarrowVerify()) {
                VideoCommon::GpuSplit::Get().NoteSyncDone(
                    VideoCommon::UnboundedCause::AcquireSync);
                return;
            }
        } else {
            narrowed.reset();
        }
        DrainBack();
        const u32 current = ReadSemaphoreWord(dma_pusher, acquire.address);
        if (narrowed) {
            VideoCommon::GpuSplit::ReportNarrowCheck(VideoCommon::DrainSite::NarrowAcquire,
                                                     *narrowed != current ? 1U : 0U, 0,
                                                     acquire.address);
        }
        if (acquire.Matches(current)) {
            return;
        }
        // Registered before the fences run, so a release among them that meets the value counts
        // even if a later one overwrites it.
        auto& waiters = Nx2SemaphoreWaiters::Get();
        if (const auto device = dma_pusher.memory_manager.GpuToCpuAddress(acquire.address)) {
            waiters.Add(this, *device, acquire.value, acquire.operation);
        }
        ReleaseFences(dma_pusher);
        if (!waiters.Latched(this) && !acquire.Matches(ReadSemaphoreWord(dma_pusher, acquire.address))) {
            pending_acquire = acquire;
            dma_pusher.suspensions.fetch_add(1, std::memory_order_release);
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::AcquireWaits, 1);
        } else {
            waiters.Remove(this);
        }
    } else {
        DrainBack();
        do {
            const u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
            regs.acquire_source = true;
            regs.acquire_value = regs.semaphore_sequence;
            if (op == GpuSemaphoreOperation::AcquireEqual) {
                regs.acquire_active = true;
                regs.acquire_mode = false;
                if (word != regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireGequal) {
                regs.acquire_active = true;
                regs.acquire_mode = true;
                if (word < regs.acquire_value) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else if (op == GpuSemaphoreOperation::AcquireMask) {
                if (word && regs.semaphore_sequence == 0) {
                    ReleaseFences(dma_pusher);
                    continue;
                }
            } else {
                LOG_ERROR(HW_GPU, "Invalid semaphore operation");
            }
        } while (false);
    }
}

void Puller::ProcessSemaphoreRelease(DmaPusher& dma_pusher) {
    const GPUVAddr sequence_address{regs.semaphore_address.SemaphoreAddress()};
    const u32 payload = regs.semaphore_release;
    OnBack([rasterizer = dma_pusher.rasterizer, sequence_address, payload] {
        rasterizer->Query(sequence_address, VideoCommon::QueryType::Payload,
                          VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
    });
    NoteRelease(dma_pusher, sequence_address, payload, true);
}

void Puller::ProcessSemaphoreAcquire(DmaPusher& dma_pusher) {
    DrainBack();
    u32 word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
    const auto value = regs.semaphore_acquire;
    while (word != value) {
        regs.acquire_active = true;
        regs.acquire_value = value;
        ReleaseFences(dma_pusher);
        word = dma_pusher.memory_manager.Read<u32>(regs.semaphore_address.SemaphoreAddress());
        // TODO(kemathe73) figure out how to do the acquire_timeout
        regs.acquire_mode = false;
        regs.acquire_source = false;
    }
}

// Ampere SEM_EXECUTE (method 0x1B): operation in bits 2:0 on the semaphore at
// SEM_ADDR_HI:SEM_ADDR_LO (methods 0x18/0x17) with payload SEM_PAYLOAD_LO (0x19).
void Puller::ProcessSemaphoreExecute(DmaPusher& dma_pusher, u32 argument) {
    const GPUVAddr address = (GPUVAddr{regs.reg_array[0x18] & 0x1FFFFFF} << 32) |
                             (regs.reg_array[0x17] & ~3U);
    const u32 payload = regs.reg_array[0x19];
    const u32 operation = argument & 7;
    if (operation == 1) { // RELEASE
        OnBack([rasterizer = dma_pusher.rasterizer, address, payload] {
            rasterizer->Query(address, VideoCommon::QueryType::Payload,
                              VideoCommon::QueryPropertiesFlags::IsAFence, payload, 0);
        });
        NoteRelease(dma_pusher, address, payload, true);
        return;
    }
    const auto met = [operation, payload](u32 word) {
        return operation == 0   ? word == payload
               : operation == 2 ? word >= payload
               : operation == 3 ? static_cast<s32>(word - payload) >= 0
               : operation == 4 ? (word & payload) != 0
               : operation == 5 ? (word & payload) == 0
                                : true;
    };
    if (PredictedMet(dma_pusher, address, met,
                     [operation, payload](u32 lower) { return operation == 2 && lower >= payload; })) {
        return;
    }
    // Acquires: check once after flushing pending fences; never block the GPU thread on them.
    // EDEN_NX2_ACQUIRE_POLL: only the warning below reads a word not met, so it doesn't drain.
    u32 word{};
    if (const std::optional<u32> polled = PollSemaphoreRead(dma_pusher, address)) {
        word = *polled;
        if (met(word)) {
            NoteAcquireSync(address);
            return;
        }
    } else {
        DrainBack();
        ReleaseFences(dma_pusher);
        word = dma_pusher.memory_manager.Read<u32>(address);
    }
    const bool ready = met(word);
    if (!ready) {
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1) < 8) {
            LOG_WARNING(HW_GPU, "SEM_EXECUTE op {} at {:#x}: {:#x} vs {:#x} not met, continuing",
                        operation, address, word, payload);
        }
    }
}

/// Calls a GPU puller method.
void Puller::CallPullerMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    static const bool nx2_trace = std::getenv("EDEN_NX2_PULLER_TRACE") != nullptr;
    if (nx2_trace) {
        LOG_INFO(HW_GPU, "nx2 puller sub={} method={:#x} arg={:#x}", method_call.subchannel,
                 method_call.method, method_call.argument);
    }
    regs.reg_array[method_call.method] = method_call.argument;
    const auto method = static_cast<BufferMethods>(method_call.method);

    switch (method) {
    case BufferMethods::BindObject: {
        ProcessBindMethod(dma_pusher, method_call);
        break;
    }
    case BufferMethods::Nop:
    case BufferMethods::SemaphoreAddressHigh:
    case BufferMethods::SemaphoreAddressLow:
    case BufferMethods::SemaphoreSequencePayload:
    case BufferMethods::SyncpointPayload:
    case BufferMethods::WrcacheFlush:
        break;
    case BufferMethods::RefCnt:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->SignalReference(); });
        break;
    case BufferMethods::SyncpointOperation:
        ProcessFenceActionMethod(dma_pusher);
        break;
    case BufferMethods::WaitForIdle:
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->WaitForIdle(); });
        VideoCommon::GpuSplit::Get().NoteSync(VideoCommon::UnboundedCause::PullerWfi);
        // The draws before it are done.
        VideoCommon::GpuSplit::Get().NoteUnboundedWrite(false,
                                                        VideoCommon::UnboundedCause::PullerWfi);
        break;
    case BufferMethods::SemaphoreOperation: {
        ProcessSemaphoreTriggerMethod(dma_pusher);
        break;
    }
    case BufferMethods::NonStallInterrupt: {
        LOG_ERROR(HW_GPU, "Special puller engine method NonStallInterrupt not implemented");
        break;
    }
    case BufferMethods::MemOpA: {
        LOG_TRACE(HW_GPU, "Memory Operation A");
        break;
    }
    case BufferMethods::MemOpB: {
        // Implement this better.
        OnBack([rasterizer = dma_pusher.rasterizer] { rasterizer->InvalidateGPUCache(); });
        VideoCommon::GpuSplit::Get().NoteUnboundedWrite(false,
                                                        VideoCommon::UnboundedCause::MemOpB);
        break;
    }
    case BufferMethods::MemOpC:
    case BufferMethods::MemOpD: {
        LOG_TRACE(HW_GPU, "Memory Operation C,D");
        break;
    }
    case BufferMethods::SemaphoreAcquire: {
        // On Ampere host classes 0x17..0x1A are SEM_ADDR_LO/HI and SEM_PAYLOAD_LO/HI.
        if (!g_nx2_ampere) {
            ProcessSemaphoreAcquire(dma_pusher);
        }
        break;
    }
    case BufferMethods::SemaphoreRelease: {
        if (g_nx2_ampere) {
            ProcessSemaphoreExecute(dma_pusher, method_call.argument);
        } else {
            ProcessSemaphoreRelease(dma_pusher);
        }
        break;
    }
    case BufferMethods::Yield: {
        // TODO(Kmather73): Research and implement this method.
        LOG_ERROR(HW_GPU, "Special puller engine method Yield not implemented");
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Special puller engine method {:X} not implemented", method);
        if (static bool dumped = false; g_nx2_ampere && !dumped) {
            dumped = true;
            dma_pusher.DumpCurrentEntry("unknown host method");
        }
        break;
    }
}

/// Calls a GPU engine method.
static bool Nx2MethodTrace() {
    static const bool trace = std::getenv("EDEN_NX2_METHOD_TRACE") != nullptr;
    return trace;
}

void Puller::CallEngineMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    const EngineID engine = bound_engines[method_call.subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 method sub={} engine={:04X} method={:#x} arg={:#x}",
                 method_call.subchannel, static_cast<u32>(engine), method_call.method,
                 method_call.argument);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMethod(dma_pusher.system, method_call.method, method_call.argument, method_call.IsLastCall());
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU engine multivalue method.
void Puller::CallEngineMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    const EngineID engine = bound_engines[subchannel];
    if (Nx2MethodTrace()) {
        LOG_INFO(HW_GPU, "nx2 multimethod sub={} engine={:04X} method={:#x} amount={} first={:#x}",
                 subchannel, static_cast<u32>(engine), method, amount, base_start[0]);
    }
    switch (engine) {
    case EngineID::FERMI_TWOD_A:
        dma_pusher.channel_state.payload->fermi_2d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_B:
        dma_pusher.channel_state.payload->maxwell_3d.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_COMPUTE_B:
        dma_pusher.channel_state.payload->kepler_compute.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::MAXWELL_DMA_COPY_A:
        dma_pusher.channel_state.payload->maxwell_dma.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::KEPLER_INLINE_TO_MEMORY_B:
        dma_pusher.channel_state.payload->kepler_memory.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    case EngineID::NV01_TIMER:
        dma_pusher.channel_state.payload->nv01_timer.CallMultiMethod(dma_pusher.system, method, base_start, amount, methods_pending);
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented engine");
        break;
    }
}

/// Calls a GPU method.
void Puller::CallMethod(DmaPusher& dma_pusher, const MethodCall& method_call) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method_call.method, method_call.subchannel);
    ASSERT(method_call.subchannel < bound_engines.size());

    if (ExecuteMethodOnEngine(dma_pusher, method_call.method)) {
        CallEngineMethod(dma_pusher, method_call);
    } else {
        CallPullerMethod(dma_pusher, method_call);
    }
}

/// Calls a GPU multivalue method.
void Puller::CallMultiMethod(DmaPusher& dma_pusher, u32 method, u32 subchannel, const u32* base_start, u32 amount, u32 methods_pending) {
    LOG_TRACE(HW_GPU, "Processing method {:08X} on subchannel {}", method, subchannel);
    ASSERT(subchannel < bound_engines.size());
    if (ExecuteMethodOnEngine(dma_pusher, method)) {
        CallEngineMultiMethod(dma_pusher, method, subchannel, base_start, amount, methods_pending);
    } else {
        for (u32 i = 0; i < amount; i++) {
            CallPullerMethod(dma_pusher, MethodCall{
                method,
                base_start[i],
                subchannel,
                methods_pending - i,
            });
        }
    }
}

/// Determines where the method should be executed.
[[nodiscard]] bool Puller::ExecuteMethodOnEngine(DmaPusher& dma_pusher, u32 method) {
    const auto buffer_method = BufferMethods(method);
    return buffer_method >= BufferMethods::NonPullerMethods;
}

} // namespace Tegra::Engines
