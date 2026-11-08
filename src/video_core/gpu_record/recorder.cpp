// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/stb.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/kernel/board/nintendo/nx/k_system_control.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/gpu_record/engine_state.h"
#include "video_core/gpu_record/recorder.h"
#include "video_core/gpu_split.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GpuRecord {

namespace {

using Clock = std::chrono::steady_clock;

/// Pages per PhysPages record of the start's snapshot (64 MiB raw, compressed in parallel).
constexpr size_t SnapshotChunkPages = 16384;
constexpr u32 NumSyncpoints = Tegra::Host1x::SyncpointManager::NUM_MAX_SYNCPOINTS;
/// Frame boundaries a start waits for a moment with no suspended channel before giving up.
constexpr u32 MaxStartAttempts = 600;

thread_local u64 t_current_ordered = 0;
thread_local bool t_starting = false;

void AppendPng(void* context, void* data, int size) {
    auto* const out = static_cast<std::vector<u8>*>(context);
    const auto* const bytes = static_cast<const u8*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

Layer ToLayer(const Tegra::FramebufferConfig& config) {
    return Layer{
        .address = config.address,
        .offset = config.offset,
        .width = config.width,
        .height = config.height,
        .stride = config.stride,
        .pixel_format = static_cast<u32>(config.pixel_format),
        .transform_flags = static_cast<u32>(config.transform_flags),
        .crop_left = config.crop_rect.left,
        .crop_top = config.crop_rect.top,
        .crop_right = config.crop_rect.right,
        .crop_bottom = config.crop_rect.bottom,
        .blending = static_cast<u32>(config.blending),
        .native_tiling = config.native_tiling,
        .layer_stack_mask = config.layer_stack_mask,
        .color_space = static_cast<u32>(config.color_space),
        .linear_unit_luminance = config.linear_unit_luminance,
    };
}

Submit ToSubmit(s32 channel, const Tegra::CommandList& list) {
    Submit submit{};
    submit.channel = channel;
    submit.command_lists.reserve(list.command_lists.size());
    for (const auto& header : list.command_lists) {
        submit.command_lists.push_back(header.raw);
    }
    submit.prefetch_command_list.reserve(list.prefetch_command_list.size());
    for (const auto& header : list.prefetch_command_list) {
        submit.prefetch_command_list.push_back(header.argument);
    }
    submit.submit_hashes = list.submit_hashes;
    submit.submitted_words.reserve(list.submitted_words.size());
    for (const auto& word : list.submitted_words) {
        submit.submitted_words.push_back(word.argument);
    }
    submit.submitted_offsets = list.submitted_offsets;
    submit.deferred_entries = list.deferred_entries;
    submit.snapshot_suspensions = list.snapshot_suspensions;
    return submit;
}

double MsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // Anonymous namespace

std::vector<u8> EncodeScreenshotPng(std::span<const u8> bgra, u32 width, u32 height,
                                    bool invert_y) {
    std::vector<u8> rgb(size_t{width} * height * 3);
    for (u32 y = 0; y < height; ++y) {
        const u8* source = bgra.data() + size_t{invert_y ? height - 1 - y : y} * width * 4;
        u8* target = rgb.data() + size_t{y} * width * 3;
        for (u32 x = 0; x < width; ++x, source += 4, target += 3) {
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
        }
    }
    std::vector<u8> png;
    if (!stbi_write_png_to_func(AppendPng, &png, static_cast<int>(width), static_cast<int>(height),
                                3, rgb.data(), static_cast<int>(width * 3))) {
        return {};
    }
    return png;
}

struct Recorder::Impl {
    enum class State { Idle, Armed, Starting, Recording, Writing, Done, Failed };

    struct PendingShot {
        u64 index;
        std::shared_ptr<std::vector<u8>> pixels;
        bool invert_y;
    };

    Core::System* system{};
    RecordOptions options;

    mutable std::mutex mutex;
    std::condition_variable gate;
    State state{State::Idle};
    RecordStatus status;
    u32 start_attempts{};
    Clock::time_point armed_at;

    FileWriter writer;
    std::vector<Event> stream;
    std::unordered_map<u64, size_t> ordered_events; ///< ordered id -> index in stream
    u64 next_ordered{1};
    std::unordered_set<u64> known_spaces;
    std::array<u32, NumSyncpoints> observed{};
    std::unordered_map<u64, u64> composite_index; ///< RequestComposite number -> index
    std::vector<PendingShot> shots;
    std::jthread write_thread;

    // Guest writes to GPU-visible pages since the last event.
    u8* physical_base{};
    size_t physical_pages{};
    std::unique_ptr<std::atomic<u64>[]> dirty_bits;
    std::mutex dirty_mutex;
    std::vector<u32> dirty_list;
    /// The pages the last event took, and their contents then: a write trapped before it
    /// landed (the trap comes first) is found by comparing them at the next event.
    std::unordered_map<u32, std::vector<u8>> recheck;

    /// Device pages this recording marked cached (UpdatePagesCachedCount +1), one bit each.
    std::vector<u64> marked;

    Tegra::MaxwellDeviceMemoryManager& Smmu() {
        return system->Host1x().MemoryManager();
    }

    /// Waits while the start runs; true when events are taken now (mutex held).
    bool EnterCpuHook(std::unique_lock<std::mutex>& lock) {
        if (t_starting) {
            return false;
        }
        if (VideoCommon::GpuSplit::OnGpuThreads()) {
            // The start waits for the GPU threads: during it, what they do comes before the
            // capture, which has it.
            return state == State::Recording;
        }
        gate.wait(lock, [this] { return state != State::Starting; });
        return state == State::Recording;
    }

    void NoteDirtyPage(u32 page) {
        std::atomic<u64>& word = dirty_bits[page / 64];
        const u64 bit = u64{1} << (page % 64);
        if ((word.load(std::memory_order_relaxed) & bit) != 0) {
            return;
        }
        if ((word.fetch_or(bit, std::memory_order_acq_rel) & bit) == 0) {
            std::scoped_lock lk{dirty_mutex};
            dirty_list.push_back(page);
        }
    }

    /// Appends the guest writes since the last event (mutex held).
    void FlushDirtyLocked() {
        std::vector<u32> pages;
        {
            std::scoped_lock lk{dirty_mutex};
            pages.swap(dirty_list);
        }
        for (u32 page : pages) {
            dirty_bits[page / 64].fetch_and(~(u64{1} << (page % 64)), std::memory_order_acq_rel);
        }
        for (const auto& [page, contents] : recheck) {
            if (std::memcmp(physical_base + u64{page} * PageSize, contents.data(), PageSize) != 0) {
                pages.push_back(page);
            }
        }
        recheck.clear();
        if (pages.empty()) {
            return;
        }
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        PhysPages event;
        event.pages = pages;
        event.data.resize(pages.size() * PageSize);
        for (size_t i = 0; i < pages.size(); ++i) {
            u8* const target = event.data.data() + i * PageSize;
            std::memcpy(target, physical_base + u64{pages[i]} * PageSize, PageSize);
            recheck.emplace(pages[i], std::vector<u8>(target, target + PageSize));
        }
        status.dirty_pages += pages.size();
        stream.emplace_back(std::move(event));
    }

    /// Appends the guest syncpoint values that changed since the last event (mutex held).
    void ObserveLocked() {
        auto& syncpoints = system->Host1x().GetSyncpointManager();
        SyncpointObserve event;
        for (u32 id = 0; id < NumSyncpoints; ++id) {
            const u32 value = syncpoints.GetGuestSyncpointValue(id);
            if (value != observed[id]) {
                observed[id] = value;
                event.values.emplace_back(id, value);
            }
        }
        if (!event.values.empty()) {
            stream.emplace_back(std::move(event));
        }
    }

    static AddressSpace DescribeSpace(const Tegra::MemoryManager& mm) {
        const auto params = mm.GetParams();
        return AddressSpace{
            .id = mm.GetID(),
            .address_space_bits = params.address_space_bits,
            .split_address = params.split_address,
            .big_page_bits = params.big_page_bits,
            .page_bits = params.page_bits,
            .native_tiling = params.native_tiling,
            .syncpoint_shim = params.syncpoint_shim,
        };
    }

    static GmmuOps SnapshotMappings(const Tegra::MemoryManager& mm) {
        GmmuOps ops{};
        mm.ForEachMapping(
            [&](GPUVAddr gpu, DAddr dev, u64 size, Tegra::PTEKind kind, bool big, bool reserved) {
                ops.ops.push_back(GmmuOp{
                    .kind = reserved ? GmmuOpKind::MapSparse : GmmuOpKind::Map,
                    .big_pages = big,
                    .pte_kind = static_cast<u8>(kind),
                    .address_space = mm.GetID(),
                    .gpu_addr = gpu,
                    .device_addr = dev,
                    .size = size,
                });
            });
        return ops;
    }

    /// An address space the stream hasn't described: its description and mappings go in at
    /// `position` (mutex held). Returns how many events were inserted.
    size_t EnsureSpace(const Tegra::MemoryManager& mm, size_t position) {
        if (!known_spaces.insert(mm.GetID()).second) {
            return 0;
        }
        std::vector<Event> events;
        events.emplace_back(DescribeSpace(mm));
        events.emplace_back(SnapshotMappings(mm));
        stream.insert(stream.begin() + static_cast<std::ptrdiff_t>(position),
                      std::make_move_iterator(events.begin()),
                      std::make_move_iterator(events.end()));
        for (auto& [id, index] : ordered_events) {
            if (index >= position) {
                index += events.size();
            }
        }
        return events.size();
    }

    /// Marks [first, first + count) device pages cached for this recording (write traps).
    void MarkPages(u64 first, u64 count) {
        u64 run_start = 0;
        u64 run_count = 0;
        const auto flush_run = [&] {
            if (run_count != 0) {
                Smmu().UpdatePagesCachedCount(run_start << PageBits, run_count << PageBits, 1);
                run_count = 0;
            }
        };
        for (u64 page = first; page < first + count; ++page) {
            u64& word = marked[page / 64];
            const u64 bit = u64{1} << (page % 64);
            if ((word & bit) != 0) {
                flush_run();
                continue;
            }
            word |= bit;
            if (run_count == 0) {
                run_start = page;
            }
            ++run_count;
        }
        flush_run();
    }

    void UnmarkPages(u64 first, u64 count) {
        u64 run_start = 0;
        u64 run_count = 0;
        const auto flush_run = [&] {
            if (run_count != 0) {
                Smmu().UpdatePagesCachedCount(run_start << PageBits, run_count << PageBits, -1);
                run_count = 0;
            }
        };
        for (u64 page = first; page < first + count && page / 64 < marked.size(); ++page) {
            u64& word = marked[page / 64];
            const u64 bit = u64{1} << (page % 64);
            if ((word & bit) == 0) {
                flush_run();
                continue;
            }
            word &= ~bit;
            if (run_count == 0) {
                run_start = page;
            }
            ++run_count;
        }
        flush_run();
    }

    void UnmarkAll() {
        for (size_t word = 0; word < marked.size(); ++word) {
            if (marked[word] != 0) {
                UnmarkPages(word * 64, 64);
            }
        }
    }

    std::string Metadata(u64 program_id) const {
        const auto& values = Settings::values;
        std::string text;
        const auto add = [&text](std::string_view key, const std::string& value) {
            text += fmt::format("{}={}\n", key, value);
        };
        add("program_id", fmt::format("{:016X}", program_id));
        add("build", Common::g_build_fullname);
        add("scm_rev", Common::g_scm_rev);
        add("switch2", Settings::CurrentProgramIsSwitch2() ? "1" : "0");
        add("nx2_driver", Service::Nvidia::Devices::g_nx2_driver.load() ? "1" : "0");
        add("nx2_ampere", Tegra::g_nx2_ampere.load() ? "1" : "0");
        add("memory_layout",
            std::to_string(static_cast<u32>(values.memory_layout_mode.GetValue())));
        add("physical_pages", std::to_string(physical_pages));
        add("frames", std::to_string(options.frames));
        add("screenshot_width", std::to_string(options.screenshot_width));
        add("screenshot_height", std::to_string(options.screenshot_height));
        add("resolution_setup",
            std::to_string(static_cast<u32>(values.resolution_setup.GetValue())));
        add("docked", Settings::IsDockedMode() ? "1" : "0");
        return text;
    }

    /// The start's capture, on the thread that owns the engines with nothing else running
    /// (GPU MapInOrder: the split front waits). False when a channel is mid-work.
    bool CaptureState(std::string& error) {
        const auto start = Clock::now();
        auto& gpu = system->GPU();
        if (gpu.HasSuspendedChannels()) {
            error = "a channel waits on a semaphore";
            return false;
        }
        bool busy = false;
        u64 program_id = 0;
        std::vector<Tegra::Control::ChannelState*> channels;
        gpu.ForEachChannel([&](Tegra::Control::ChannelState& channel) {
            if (!channel.initialized || !channel.payload || channel.memory_manager == nullptr) {
                return;
            }
            const auto& pusher = channel.payload->dma_pusher;
            if (!pusher.dma_pushbuffer.empty() || pusher.dma_state.method_count != 0 ||
                !pusher.parked_methods.empty()) {
                busy = true;
            }
            program_id = channel.program_id;
            channels.push_back(&channel);
        });
        if (busy) {
            error = "a channel has queued commands";
            return false;
        }
        if (channels.empty()) {
            error = "no GPU channel";
            return false;
        }
        std::sort(channels.begin(), channels.end(),
                  [](const auto* a, const auto* b) { return a->bind_id < b->bind_id; });

        // Device mappings, contiguous on both sides.
        SmmuMap smmu{};
        auto& device = Smmu();
        device.ForEachMappedPage([&](size_t device_page, u32 phys_page) {
            if (!smmu.runs.empty()) {
                SmmuRun& last = smmu.runs.back();
                if (last.device_page + last.count == device_page &&
                    last.phys_page + last.count == phys_page) {
                    ++last.count;
                    return;
                }
            }
            smmu.runs.push_back(SmmuRun{device_page, phys_page, 1});
        });

        // From here on every guest write to these pages traps into OnCpuWrite.
        Detail::tracking.store(true, std::memory_order_seq_cst);
        for (const SmmuRun& run : smmu.runs) {
            MarkPages(run.device_page, run.count);
        }

        // GPU-written data the guest memory doesn't have yet.
        auto* const rasterizer = gpu.Renderer().ReadRasterizer();
        {
            u64 begin = 0;
            u64 end = 0;
            for (const SmmuRun& run : smmu.runs) {
                if (run.device_page == end) {
                    end += run.count;
                    continue;
                }
                if (end != begin) {
                    rasterizer->FlushRegion(begin << PageBits, (end - begin) << PageBits);
                }
                begin = run.device_page;
                end = begin + run.count;
            }
            if (end != begin) {
                rasterizer->FlushRegion(begin << PageBits, (end - begin) << PageBits);
            }
        }

        std::vector<Event> init;
        init.emplace_back(smmu);
        for (const auto& [address, size] : device.ZeroReadRanges()) {
            init.emplace_back(ZeroReadRange{address, size});
        }
        for (auto* channel : channels) {
            const auto& mm = *channel->memory_manager;
            if (known_spaces.insert(mm.GetID()).second) {
                init.emplace_back(DescribeSpace(mm));
                init.emplace_back(SnapshotMappings(mm));
            }
        }
        for (auto* channel : channels) {
            init.emplace_back(
                Channel{channel->bind_id, channel->memory_manager->GetID(), channel->program_id});
        }
        init.emplace_back(MacroContext{CaptureSharedState(*channels.front())});
        for (auto* channel : channels) {
            init.emplace_back(EngineState{channel->bind_id, CaptureChannel(*channel)});
        }
        auto& syncpoints = system->Host1x().GetSyncpointManager();
        Syncpoints values;
        for (u32 id = 0; id < NumSyncpoints; ++id) {
            values.guest.push_back(syncpoints.GetGuestSyncpointValue(id));
            values.host.push_back(syncpoints.GetHostSyncpointValue(id));
            observed[id] = values.guest.back();
        }
        init.emplace_back(std::move(values));

        Header header{};
        header.metadata = Metadata(program_id);
        if (!writer.Open(options.path, header)) {
            error = fmt::format("cannot write {}", options.path.string());
            return false;
        }

        // Every physical page a device page maps to, after the flush above.
        std::vector<u32> pages;
        {
            std::vector<u64> seen((physical_pages + 63) / 64);
            for (const SmmuRun& run : smmu.runs) {
                for (u32 i = 0; i < run.count; ++i) {
                    const u32 page = run.phys_page + i;
                    if (page >= physical_pages) {
                        continue;
                    }
                    u64& word = seen[page / 64];
                    const u64 bit = u64{1} << (page % 64);
                    if ((word & bit) == 0) {
                        word |= bit;
                        pages.push_back(page);
                    }
                }
            }
        }
        std::sort(pages.begin(), pages.end());
        status.snapshot_pages = pages.size();
        const size_t chunks = (pages.size() + SnapshotChunkPages - 1) / SnapshotChunkPages;
        const size_t workers = std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 1, 8);
        bool written = true;
        for (size_t first = 0; first < chunks && written; first += workers) {
            const size_t last = std::min(chunks, first + workers);
            std::vector<std::future<std::pair<std::vector<u8>, u64>>> jobs;
            for (size_t chunk = first; chunk < last; ++chunk) {
                jobs.push_back(std::async(std::launch::async, [this, &pages, chunk] {
                    const size_t begin = chunk * SnapshotChunkPages;
                    const size_t end = std::min(pages.size(), begin + SnapshotChunkPages);
                    PhysPages event;
                    event.pages.assign(pages.begin() + static_cast<std::ptrdiff_t>(begin),
                                       pages.begin() + static_cast<std::ptrdiff_t>(end));
                    event.data.resize(event.pages.size() * PageSize);
                    for (size_t i = 0; i < event.pages.size(); ++i) {
                        std::memcpy(event.data.data() + i * PageSize,
                                    physical_base + u64{event.pages[i]} * PageSize, PageSize);
                    }
                    const std::vector<u8> payload = EncodeEvent(Event{std::move(event)});
                    return std::make_pair(CompressPayload(payload), u64{payload.size()});
                }));
            }
            for (auto& job : jobs) {
                auto [compressed, raw_size] = job.get();
                written = written &&
                          writer.WriteEncoded(EventType::PhysPages, compressed, raw_size, true);
            }
        }
        for (const Event& event : init) {
            written = written && writer.Write(event);
        }
        written = written && writer.Write(Event{InitEnd{}});
        if (!written) {
            error = fmt::format("cannot write {}", options.path.string());
            return false;
        }
        status.start_ms = MsSince(start);
        return true;
    }

    /// GPU-ordered events whose operation hasn't run cannot be written yet: the writer waits
    /// for the GPU to go idle first.
    void Finish() {
        const auto start = Clock::now();
        system->GPU().WaitForIdle();
        std::unique_lock lock{mutex};
        Detail::recording.store(false, std::memory_order_seq_cst);
        bool ok = true;
        // Screenshots go right after their composite.
        std::unordered_map<u64, size_t> shot_of;
        for (size_t i = 0; i < shots.size(); ++i) {
            shot_of.emplace(shots[i].index, i);
        }
        for (const Event& event : stream) {
            if (const auto* gmmu = std::get_if<GmmuOps>(&event);
                gmmu != nullptr && gmmu->ordered_id != 0 && gmmu->ops.empty() &&
                !gmmu->before_later_reads) {
                continue; // An ordered operation that changed no mapping.
            }
            ok = ok && writer.Write(event);
            if (const auto* composite = std::get_if<Composite>(&event)) {
                if (const auto it = shot_of.find(composite->index); it != shot_of.end()) {
                    const PendingShot& shot = shots[it->second];
                    Screenshot screenshot{
                        .composite_index = shot.index,
                        .width = options.screenshot_width,
                        .height = options.screenshot_height,
                        .png = EncodeScreenshotPng(*shot.pixels, options.screenshot_width,
                                                   options.screenshot_height, shot.invert_y),
                    };
                    ok = ok && writer.Write(Event{std::move(screenshot)});
                }
            }
        }
        status.bytes = writer.BytesWritten();
        ok = writer.Close() && ok;
        stream.clear();
        shots.clear();
        recheck.clear();
        status.write_ms = MsSince(start);
        state = ok ? State::Done : State::Failed;
        status.state = ok ? "done" : "failed";
        if (!ok) {
            status.error = "writing the recording failed";
        }
        LOG_INFO(HW_GPU,
                 "GPU recording {}: {} frames, {} submits, {} composites, {} snapshot pages, {} "
                 "written pages, {} MiB, start {:.0f} ms, write {:.0f} ms",
                 status.state, status.frames, status.submits, status.composites,
                 status.snapshot_pages, status.dirty_pages, status.bytes >> 20, status.start_ms,
                 status.write_ms);
    }

    /// Ends the recording at a frame boundary (mutex held).
    void StopLocked() {
        state = State::Writing;
        status.state = "writing";
        Detail::tracking.store(false, std::memory_order_seq_cst);
        UnmarkAll();
        gate.notify_all();
        write_thread = std::jthread([this] {
            Common::SetCurrentThreadName("GpuRecordWriter");
            Finish();
        });
    }

    void Start() {
        Detail::armed.store(false, std::memory_order_seq_cst);
        {
            std::scoped_lock lk{mutex};
            state = State::Starting;
            status.state = "starting";
            known_spaces.clear();
            stream.clear();
            ordered_events.clear();
            composite_index.clear();
            shots.clear();
            recheck.clear();
            marked.assign(
                ((u64{1} << Tegra::MaxwellDeviceTraits::device_virtual_bits) >> PageBits) / 64, 0);
            physical_pages =
                Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize() >>
                PageBits;
            physical_base = system->DeviceMemory().buffer.BackingBasePointer();
            dirty_bits = std::make_unique<std::atomic<u64>[]>((physical_pages + 63) / 64);
            dirty_list.clear();
        }
        Detail::recording.store(true, std::memory_order_seq_cst);

        std::promise<bool> done;
        std::string error;
        t_starting = true;
        system->GPU().MapInOrder(0, 0,
                                 [this, &done, &error] { done.set_value(CaptureState(error)); });
        const bool ok = done.get_future().get();
        t_starting = false;

        std::scoped_lock lk{mutex};
        if (!ok) {
            Detail::recording.store(false, std::memory_order_seq_cst);
            if (Detail::tracking.exchange(false)) {
                UnmarkAll();
            }
            static_cast<void>(writer.Close());
            if (++start_attempts < MaxStartAttempts && error != "no GPU channel" &&
                error.rfind("cannot write", 0) != 0) {
                state = State::Armed;
                status.state = "armed";
                status.error = error;
                Detail::armed.store(true, std::memory_order_seq_cst);
            } else {
                state = State::Failed;
                status.state = "failed";
                status.error = error;
                LOG_ERROR(HW_GPU, "GPU recording could not start: {}", error);
            }
            gate.notify_all();
            return;
        }
        state = State::Recording;
        status.state = "recording";
        status.error.clear();
        stream.emplace_back(FrameBoundary{0});
        LOG_INFO(HW_GPU, "GPU recording started after {} attempts: {} pages, {:.0f} ms",
                 start_attempts + 1, status.snapshot_pages, status.start_ms);
        gate.notify_all();
    }
};

Recorder::Recorder() : impl{std::make_unique<Impl>()} {
    impl->status.state = "idle";
}

Recorder::~Recorder() = default;

Recorder& Recorder::Get() {
    static Recorder recorder;
    return recorder;
}

bool Recorder::Arm(Core::System& system, const RecordOptions& options, std::string& error) {
    std::scoped_lock lk{impl->mutex};
    using State = Impl::State;
    if (impl->state == State::Armed || impl->state == State::Starting ||
        impl->state == State::Recording || impl->state == State::Writing) {
        error = "a recording is already " + impl->status.state;
        return false;
    }
    if (options.frames == 0 || options.path.empty()) {
        error = "frames must be > 0 and path given";
        return false;
    }
    if (impl->write_thread.joinable()) {
        impl->write_thread.join();
    }
    impl->system = &system;
    impl->options = options;
    impl->status = RecordStatus{};
    impl->status.state = "armed";
    impl->status.path = options.path.string();
    impl->start_attempts = 0;
    impl->state = State::Armed;
    Detail::armed.store(true, std::memory_order_seq_cst);
    return true;
}

RecordStatus Recorder::GetStatus() const {
    std::scoped_lock lk{impl->mutex};
    return impl->status;
}

void Recorder::OnFrameBoundary() {
    if (Detail::armed.load(std::memory_order_acquire)) {
        impl->Start();
        return;
    }
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock)) {
        return;
    }
    impl->FlushDirtyLocked();
    impl->ObserveLocked();
    impl->stream.emplace_back(FrameBoundary{++impl->status.frames});
    if (impl->status.frames >= impl->options.frames) {
        impl->StopLocked();
    }
}

void Recorder::OnSubmit(s32 channel, const Tegra::CommandList& entries) {
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock)) {
        return;
    }
    impl->FlushDirtyLocked();
    impl->ObserveLocked();
    impl->stream.emplace_back(ToSubmit(channel, entries));
    ++impl->status.submits;
}

void Recorder::OnComposite(const std::vector<Tegra::FramebufferConfig>& layers,
                           const std::vector<Service::Nvidia::NvFence>& fences, u64 request) {
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock)) {
        return;
    }
    impl->FlushDirtyLocked();
    impl->ObserveLocked();
    Composite composite{};
    composite.index = impl->status.composites++;
    for (const auto& layer : layers) {
        composite.layers.push_back(ToLayer(layer));
    }
    for (const auto& fence : fences) {
        composite.fences.emplace_back(fence.id, fence.value);
    }
    impl->composite_index.emplace(request, composite.index);
    impl->stream.emplace_back(std::move(composite));
}

void Recorder::OnInitChannel(const Tegra::Control::ChannelState& channel) {
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock) || channel.memory_manager == nullptr) {
        return;
    }
    impl->FlushDirtyLocked();
    impl->EnsureSpace(*channel.memory_manager, impl->stream.size());
    impl->stream.emplace_back(
        Channel{channel.bind_id, channel.memory_manager->GetID(), channel.program_id});
}

void Recorder::OnSmmuMap(u64 address, u64 size) {
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock)) {
        return;
    }
    impl->FlushDirtyLocked();
    SmmuMap map{};
    PhysPages contents;
    auto& device = impl->Smmu();
    const u64 first = address >> PageBits;
    const u64 count = (size + PageSize - 1) >> PageBits;
    for (u64 page = first; page < first + count; ++page) {
        const PAddr raw = device.GetPhysicalRawAddressFromDAddr(page << PageBits);
        if (raw == 0 && page != 0) {
            continue;
        }
        const u32 phys_page = static_cast<u32>(raw >> PageBits);
        if (!map.runs.empty()) {
            SmmuRun& last = map.runs.back();
            if (last.device_page + last.count == page && last.phys_page + last.count == phys_page) {
                ++last.count;
                contents.pages.push_back(phys_page);
                continue;
            }
        }
        map.runs.push_back(SmmuRun{page, phys_page, 1});
        contents.pages.push_back(phys_page);
    }
    if (map.runs.empty()) {
        return;
    }
    // The pages' contents now, and their later writes through the traps.
    contents.data.resize(contents.pages.size() * PageSize);
    for (size_t i = 0; i < contents.pages.size(); ++i) {
        std::memcpy(contents.data.data() + i * PageSize,
                    impl->physical_base + u64{contents.pages[i]} * PageSize, PageSize);
    }
    for (const SmmuRun& run : map.runs) {
        impl->MarkPages(run.device_page, run.count);
    }
    impl->stream.emplace_back(std::move(contents));
    impl->stream.emplace_back(std::move(map));
}

void Recorder::OnSmmuUnmap(u64 address, u64 size) {
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock)) {
        return;
    }
    impl->FlushDirtyLocked();
    impl->UnmarkPages(address >> PageBits, (size + PageSize - 1) >> PageBits);
    impl->stream.emplace_back(SmmuUnmap{address, size});
}

void Recorder::OnGmmuOp(const Tegra::MemoryManager& memory_manager, GmmuOp op) {
    op.address_space = memory_manager.GetID();
    std::scoped_lock lock{impl->mutex};
    using State = Impl::State;
    if (t_current_ordered != 0) {
        // Inside a recorded GPU-ordered operation: the op belongs to it.
        const auto it = impl->ordered_events.find(t_current_ordered);
        if (it == impl->ordered_events.end()) {
            return;
        }
        const size_t inserted = impl->EnsureSpace(memory_manager, it->second);
        auto& event = std::get<GmmuOps>(impl->stream[it->second]);
        static_cast<void>(inserted);
        event.ops.push_back(op);
        return;
    }
    // Made at once by its caller. During the start it ran before the capture (the GPU thread
    // works through what was queued before it), so the snapshot has it.
    if (impl->state != State::Recording) {
        return;
    }
    impl->FlushDirtyLocked();
    impl->EnsureSpace(memory_manager, impl->stream.size());
    if (!impl->stream.empty()) {
        if (auto* last = std::get_if<GmmuOps>(&impl->stream.back());
            last != nullptr && last->ordered_id == 0) {
            last->ops.push_back(op);
            return;
        }
    }
    impl->stream.emplace_back(GmmuOps{0, false, {op}});
}

void Recorder::OnSyncpointIncrement(u32 id, bool guest, bool host) {
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock)) {
        return;
    }
    impl->FlushDirtyLocked();
    impl->stream.emplace_back(SyncpointIncrement{id, guest, host});
}

std::function<void()> Recorder::WrapOrdered(std::function<void()>&& func, bool before_later_reads) {
    std::unique_lock lock{impl->mutex};
    if (!impl->EnterCpuHook(lock)) {
        return std::move(func);
    }
    impl->FlushDirtyLocked();
    const u64 id = impl->next_ordered++;
    impl->ordered_events.emplace(id, impl->stream.size());
    impl->stream.emplace_back(GmmuOps{id, before_later_reads, {}});
    return [id, inner = std::move(func)] {
        t_current_ordered = id;
        inner();
        t_current_ordered = 0;
    };
}

void Recorder::OnCpuWrite(const u8* host_pointer, size_t size) {
    if (impl->dirty_bits == nullptr || size == 0) {
        return;
    }
    const u64 raw = static_cast<u64>(host_pointer - impl->physical_base);
    const u64 first = raw >> PageBits;
    const u64 last = (raw + size - 1) >> PageBits;
    for (u64 page = first; page <= last && page < impl->physical_pages; ++page) {
        impl->NoteDirtyPage(static_cast<u32>(page));
    }
}

void Recorder::OnCompositeExecuted(u64 request) {
    std::scoped_lock lock{impl->mutex};
    using State = Impl::State;
    if (impl->state != State::Recording && impl->state != State::Writing) {
        return;
    }
    const auto it = impl->composite_index.find(request);
    if (it == impl->composite_index.end() || impl->options.screenshot_every == 0 ||
        it->second % impl->options.screenshot_every != 0) {
        return;
    }
    auto& renderer = impl->system->GPU().Renderer();
    if (renderer.IsScreenshotPending()) {
        return;
    }
    const u32 width = impl->options.screenshot_width;
    const u32 height = impl->options.screenshot_height;
    auto pixels = std::make_shared<std::vector<u8>>(size_t{width} * height * 4);
    const u64 index = it->second;
    renderer.RequestScreenshot(
        pixels->data(),
        [this, pixels, index](bool invert_y) {
            std::scoped_lock lk{impl->mutex};
            impl->shots.push_back({index, pixels, invert_y});
        },
        Layout::DefaultFrameLayout(width, height));
}

} // namespace VideoCommon::GpuRecord
