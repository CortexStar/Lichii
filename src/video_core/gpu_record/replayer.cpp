// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/page_table.h"
#include "common/scratch_buffer.h"
#include "common/thread_cpu_clock.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/memory.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/gpu_record/engine_state.h"
#include "video_core/gpu_record/record_format.h"
#include "video_core/gpu_record/recorder.h"
#include "video_core/gpu_record/replayer.h"
#include "video_core/guest_frame_timing.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/native_verify.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GpuRecord {

namespace {

using Clock = std::chrono::steady_clock;

/// How long a step waits for the GPU progress the guest had seen before it gives up waiting.
constexpr auto SyncTimeout = std::chrono::seconds{5};
/// How long the end of a loop waits for its last composite.
constexpr auto DrainTimeout = std::chrono::seconds{30};
/// The replay's stand-in process maps guest physical memory at its physical address.
constexpr u64 AddressSpaceBits = 39;

double Ms(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

Tegra::CommandList ToCommandList(const Submit& submit) {
    Tegra::CommandList list;
    list.command_lists.reserve(submit.command_lists.size());
    for (const u64 raw : submit.command_lists) {
        Tegra::CommandListHeader header{};
        header.raw = raw;
        list.command_lists.push_back(header);
    }
    list.prefetch_command_list.reserve(submit.prefetch_command_list.size());
    for (const u32 word : submit.prefetch_command_list) {
        Tegra::CommandHeader header{};
        header.argument = word;
        list.prefetch_command_list.push_back(header);
    }
    list.submit_hashes = submit.submit_hashes;
    list.submitted_words.reserve(submit.submitted_words.size());
    for (const u32 word : submit.submitted_words) {
        Tegra::CommandHeader header{};
        header.argument = word;
        list.submitted_words.push_back(header);
    }
    list.submitted_offsets = submit.submitted_offsets;
    list.deferred_entries = submit.deferred_entries;
    list.snapshot_suspensions = submit.snapshot_suspensions;
    return list;
}

Tegra::FramebufferConfig ToFramebuffer(const Layer& layer) {
    const auto frame = VideoCore::GuestFrameTiming::Current();
    return Tegra::FramebufferConfig{
        .address = layer.address,
        .offset = layer.offset,
        .width = layer.width,
        .height = layer.height,
        .stride = layer.stride,
        .pixel_format = static_cast<Service::android::PixelFormat>(layer.pixel_format),
        .transform_flags =
            static_cast<Service::android::BufferTransformFlags>(layer.transform_flags),
        .crop_rect = {layer.crop_left, layer.crop_top, layer.crop_right, layer.crop_bottom},
        .blending = static_cast<Tegra::BlendMode>(layer.blending),
        .native_tiling = layer.native_tiling,
        .layer_stack_mask = layer.layer_stack_mask,
        .color_space = static_cast<Tegra::ColorSpace>(layer.color_space),
        .guest_frame = frame.id,
        .guest_start_ns = frame.start_ns,
        .linear_unit_luminance = layer.linear_unit_luminance,
        .input_ns = 0,
    };
}

bool WriteFile(const std::filesystem::path& path, std::span<const u8> bytes) {
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

std::string JsonString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

class Replayer {
public:
    Replayer(Core::System& system_, Core::Frontend::EmuWindow& window_,
             const ReplayOptions& options_)
        : system{system_}, window{window_}, options{options_} {}

    /// Before the system shuts down: the stand-in process leaves the device memory manager.
    void ReleaseProcess() {
        if (asid_registered) {
            system.Host1x().MemoryManager().UnregisterProcess(asid);
            asid_registered = false;
        }
    }

    ReplayReport Run() {
        const auto setup_start = Clock::now();
        std::string error;
        auto read = ReadFile(options.path, error);
        if (!read) {
            return Fail(fmt::format("{}: {}", options.path.string(), error));
        }
        recording = std::move(*read);
        report.metadata = recording.header.metadata;
        report.events = recording.events.size();
        std::error_code ec;
        std::filesystem::create_directories(options.out_dir, ec);

        const auto& metadata = recording.header.metadata;
        program_id = std::strtoull(MetadataValue(metadata, "program_id").c_str(), nullptr, 16);
        const bool switch2 = MetadataValue(metadata, "switch2") == "1";
        shot_width = static_cast<u32>(
            std::strtoul(MetadataValue(metadata, "screenshot_width").c_str(), nullptr, 10));
        shot_height = static_cast<u32>(
            std::strtoul(MetadataValue(metadata, "screenshot_height").c_str(), nullptr, 10));

        if (system.InitializeGpuReplay(window, program_id, switch2) !=
            Core::SystemResultStatus::Success) {
            return Fail("the GPU could not be set up");
        }
        Service::Nvidia::Devices::g_nx2_driver = MetadataValue(metadata, "nx2_driver") == "1";
        Tegra::g_nx2_ampere = MetadataValue(metadata, "nx2_ampere") == "1";
        physical_base = system.DeviceMemory().buffer.BackingBasePointer();
        physical_pages =
            std::strtoull(MetadataValue(metadata, "physical_pages").c_str(), nullptr, 10);

        auto& gpu = system.GPU();
        gpu.SetCompositeObserver([this](u64 request) { OnComposite(request); });
        gpu.Start();
        if (options.disk_cache) {
            gpu.Renderer().ReadRasterizer()->LoadDiskResources(
                program_id, std::stop_token{}, [](VideoCore::LoadCallbackStage, size_t, size_t) {});
        }

        // The guest process's stand-in: guest physical memory at its own address.
        page_table.Resize(AddressSpaceBits, Core::Memory::YUZU_PAGEBITS);
        process_memory = std::make_unique<Core::Memory::Memory>(system);
        process_memory->SetCurrentPageTable(page_table);
        process_memory->MapMemoryRegion(page_table, Core::DramMemoryMap::Base,
                                        physical_pages << PageBits,
                                        Common::PhysicalAddress{Core::DramMemoryMap::Base},
                                        Common::MemoryPermission::ReadWrite, false);
        asid = system.Host1x().MemoryManager().RegisterProcess(process_memory.get());
        asid_registered = true;

        if (!ApplyInit(error)) {
            return Fail(error);
        }
        report.setup_ms = Ms(Clock::now() - setup_start);
        LOG_INFO(HW_GPU, "GPU replay: {} events, set up in {:.0f} ms", report.events,
                 report.setup_ms);

        const u32 loops = std::max<u32>(options.loops, 1);
        if (loops > 1 && stream_changes_state) {
            report.error = "the stream changes mappings or channels: one loop only";
        }
        const u32 run_loops = stream_changes_state ? 1 : loops;
        for (u32 loop = 0; loop < run_loops; ++loop) {
            if (loop != 0 && !Rewind(error)) {
                return Fail(error);
            }
            current_loop = loop;
            capture = options.screenshots && loop == 0;
            const auto loop_start = Clock::now();
            if (!PlayStream(error)) {
                return Fail(error);
            }
            WaitForComposites(u64{loop + 1} * composites_per_loop);
            report.loop_ms.push_back(Ms(Clock::now() - loop_start));
            LOG_INFO(HW_GPU, "GPU replay loop {}: {} composites in {:.1f} ms", loop,
                     composites_per_loop, report.loop_ms.back());
        }
        gpu.WaitForIdle();
        {
            std::unique_lock lock{shot_mutex};
            shot_cv.wait_for(lock, std::chrono::seconds{5}, [this] { return shots_pending == 0; });
        }
        for (u32 layer = 0; layer < static_cast<u32>(NativeVerify::Layer::Count); ++layer) {
            const auto& counters =
                NativeVerify::CountersOf(static_cast<NativeVerify::Layer>(layer));
            const std::string name{NativeVerify::layer_names[layer]};
            report.verify_checks.emplace_back(name, counters.checks.load());
            report.verify_differences.emplace_back(name, counters.differences.load());
        }
        {
            std::scoped_lock lock{timing_mutex};
            report.frames = frames;
        }
        report.composites = composites_per_loop;
        report.ok = true;
        return report;
    }

private:
    ReplayReport Fail(std::string error) {
        LOG_ERROR(HW_GPU, "GPU replay failed: {}", error);
        report.ok = false;
        report.error = std::move(error);
        return report;
    }

    Tegra::MaxwellDeviceMemoryManager& Smmu() {
        return system.Host1x().MemoryManager();
    }

    void WritePages(const PhysPages& pages, bool as_cpu_write) {
        auto& gpu = system.GPU();
        auto& dirty = system.GetGPUDirtyMemoryManager()[0];
        for (size_t i = 0; i < pages.pages.size(); ++i) {
            const u64 raw = u64{pages.pages[i]} << PageBits;
            if (as_cpu_write) {
                // As a trapped guest write: the caches hear first (Core::Memory's write trap).
                Smmu().ApplyOpOnPAddr(raw, scratch, [&](DAddr address) {
                    if (gpu.OnCPUWrite(address, PageSize)) {
                        dirty.Collect(address, PageSize);
                    }
                });
            }
            std::memcpy(physical_base + raw, pages.data.data() + i * PageSize, PageSize);
        }
    }

    void MapSmmu(const SmmuMap& map) {
        for (const SmmuRun& run : map.runs) {
            Smmu().Map(run.device_page << PageBits,
                       Core::DramMemoryMap::Base + (u64{run.phys_page} << PageBits),
                       u64{run.count} << PageBits, asid, true);
        }
    }

    Tegra::MemoryManager* Space(u64 id) {
        const auto it = spaces.find(id);
        return it != spaces.end() ? it->second.get() : nullptr;
    }

    void CreateSpace(const AddressSpace& space) {
        auto mm = std::make_unique<Tegra::MemoryManager>(system, Smmu(), space.address_space_bits,
                                                         space.split_address, space.big_page_bits,
                                                         space.page_bits);
        system.GPU().InitAddressSpace(*mm);
        if (space.native_tiling) {
            mm->EnableNativeTiling();
        }
        if (space.syncpoint_shim != 0) {
            mm->SetNativeSyncpointShim(space.syncpoint_shim);
        }
        spaces[space.id] = std::move(mm);
    }

    void ApplyGmmu(const std::vector<GmmuOp>& ops) {
        for (const GmmuOp& op : ops) {
            Tegra::MemoryManager* const mm = Space(op.address_space);
            if (mm == nullptr) {
                LOG_ERROR(HW_GPU, "GPU replay: map in unknown address space {}", op.address_space);
                continue;
            }
            switch (op.kind) {
            case GmmuOpKind::Map:
                mm->Map(op.gpu_addr, op.device_addr, op.size,
                        static_cast<Tegra::PTEKind>(op.pte_kind), op.big_pages);
                break;
            case GmmuOpKind::MapSparse:
                mm->MapSparse(op.gpu_addr, op.size, op.big_pages);
                break;
            case GmmuOpKind::Unmap:
                mm->Unmap(op.gpu_addr, op.size);
                break;
            }
        }
    }

    bool CreateChannel(const Channel& recorded, std::string& error) {
        Tegra::MemoryManager* const mm = Space(recorded.address_space);
        if (mm == nullptr) {
            error = fmt::format("channel {} uses an unknown address space", recorded.bind_id);
            return false;
        }
        auto& gpu = system.GPU();
        // Channel ids are handed out in order: make the recorded one.
        while (channels.empty() || channels.rbegin()->first < recorded.bind_id) {
            auto channel = gpu.AllocateChannel();
            const s32 id = channel->bind_id;
            channels.emplace(id, std::move(channel));
            if (id > recorded.bind_id) {
                break;
            }
        }
        const auto it = channels.find(recorded.bind_id);
        if (it == channels.end()) {
            error = fmt::format("channel {} cannot be made again", recorded.bind_id);
            return false;
        }
        it->second->memory_manager = mm;
        gpu.InitChannel(*it->second, recorded.program_id);
        return true;
    }

    /// The recorded state up to InitEnd.
    bool ApplyInit(std::string& error) {
        auto& gpu = system.GPU();
        size_t index = 0;
        std::vector<GmmuOp> init_maps;
        for (; index < recording.events.size(); ++index) {
            const Event& event = recording.events[index];
            if (std::holds_alternative<InitEnd>(event)) {
                ++index;
                break;
            }
            if (const auto* pages = std::get_if<PhysPages>(&event)) {
                WritePages(*pages, false);
                initial_pages.push_back(pages);
            } else if (const auto* map = std::get_if<SmmuMap>(&event)) {
                MapSmmu(*map);
                smmu_runs.insert(smmu_runs.end(), map->runs.begin(), map->runs.end());
            } else if (const auto* zero = std::get_if<ZeroReadRange>(&event)) {
                Smmu().AddZeroReadRange(zero->address, zero->size);
            } else if (const auto* space = std::get_if<AddressSpace>(&event)) {
                CreateSpace(*space);
            } else if (const auto* gmmu = std::get_if<GmmuOps>(&event)) {
                init_maps.insert(init_maps.end(), gmmu->ops.begin(), gmmu->ops.end());
            } else if (const auto* channel = std::get_if<Channel>(&event)) {
                // The mappings first: InitChannel doesn't read them, but keep the order.
                if (!init_maps.empty()) {
                    RunOnGpu([this, maps = std::move(init_maps)] { ApplyGmmu(maps); });
                    init_maps.clear();
                }
                if (!CreateChannel(*channel, error)) {
                    return false;
                }
                init_channels.push_back(channel->bind_id);
            } else if (const auto* macro = std::get_if<MacroContext>(&event)) {
                shared_state = &macro->blob;
            } else if (const auto* state = std::get_if<EngineState>(&event)) {
                engine_states.emplace_back(state->bind_id, &state->blob);
            } else if (const auto* values = std::get_if<Syncpoints>(&event)) {
                initial_syncpoints = values;
            } else {
                error = "unexpected record before the end of the recorded state";
                return false;
            }
        }
        if (!init_maps.empty()) {
            RunOnGpu([this, maps = std::move(init_maps)] { ApplyGmmu(maps); });
        }
        stream_begin = index;
        if (channels.empty() || shared_state == nullptr || initial_syncpoints == nullptr) {
            error = "the recording has no GPU state";
            return false;
        }
        for (size_t i = stream_begin; i < recording.events.size(); ++i) {
            const Event& event = recording.events[i];
            if (std::holds_alternative<Composite>(event)) {
                ++composites_per_loop;
            } else if (std::holds_alternative<Submit>(event)) {
                ++report.submits;
            } else if (const auto* shot = std::get_if<Screenshot>(&event)) {
                recorded_shots.emplace(shot->composite_index, shot);
            } else if (std::holds_alternative<SmmuMap>(event) ||
                       std::holds_alternative<SmmuUnmap>(event) ||
                       std::holds_alternative<AddressSpace>(event) ||
                       std::holds_alternative<Channel>(event)) {
                stream_changes_state = true;
            } else if (const auto* gmmu = std::get_if<GmmuOps>(&event);
                       gmmu && !gmmu->ops.empty()) {
                stream_changes_state = true;
            }
        }
        if (!RestoreEngines(error)) {
            return false;
        }
        gpu.WaitForIdle();
        return true;
    }

    /// Engine state, shared macro state and syncpoints as recorded, on the engines' thread.
    bool RestoreEngines(std::string& error) {
        bool ok = true;
        RunOnGpu([this, &ok, &error] {
            const auto first = channels.find(engine_states.empty() ? init_channels.front()
                                                                   : engine_states.front().first);
            if (first == channels.end() ||
                !RestoreSharedState(*first->second, *shared_state, error)) {
                ok = false;
                return;
            }
            for (const auto& [bind_id, blob] : engine_states) {
                const auto it = channels.find(bind_id);
                if (it == channels.end() || !RestoreChannel(*it->second, *blob, error)) {
                    error = fmt::format("channel {}: {}", bind_id, error);
                    ok = false;
                    return;
                }
            }
            system.Host1x().GetSyncpointManager().RestoreValues(initial_syncpoints->guest,
                                                                initial_syncpoints->host);
        });
        return ok;
    }

    /// Runs func on the thread that owns the engines, after everything queued, and waits.
    template <typename Func>
    void RunOnGpu(Func&& func) {
        std::promise<void> done;
        system.GPU().MapInOrder(0, 0, [&func, &done] {
            func();
            done.set_value();
        });
        done.get_future().wait();
    }

    /// Back to the recorded start for another loop: GPU-written data to memory, memory as
    /// recorded (the pages that differ, as guest writes the caches hear about), engines and
    /// syncpoints as recorded.
    bool Rewind(std::string& error) {
        system.GPU().WaitForIdle();
        RunOnGpu([this] {
            auto* const rasterizer = system.GPU().Renderer().ReadRasterizer();
            for (const SmmuRun& run : smmu_runs) {
                rasterizer->FlushRegion(run.device_page << PageBits, u64{run.count} << PageBits);
            }
            for (const PhysPages* pages : initial_pages) {
                for (size_t i = 0; i < pages->pages.size(); ++i) {
                    const u64 raw = u64{pages->pages[i]} << PageBits;
                    const u8* const recorded = pages->data.data() + i * PageSize;
                    if (std::memcmp(physical_base + raw, recorded, PageSize) == 0) {
                        continue;
                    }
                    std::memcpy(physical_base + raw, recorded, PageSize);
                    Smmu().ApplyOpOnPAddr(raw, scratch, [&](DAddr address) {
                        rasterizer->OnCacheInvalidation(address, PageSize);
                    });
                }
            }
        });
        return RestoreEngines(error);
    }

    void WaitObserved(const SyncpointObserve& observe) {
        if (options.sync == ReplaySync::NoWait) {
            return;
        }
        auto& syncpoints = system.Host1x().GetSyncpointManager();
        for (const auto& [id, value] : observe.values) {
            const auto deadline = Clock::now() + SyncTimeout;
            u32 spins = 0;
            while (!syncpoints.IsReadyGuest(id, value)) {
                if (Clock::now() > deadline) {
                    if (sync_timeouts++ < 8) {
                        LOG_WARNING(HW_GPU,
                                    "GPU replay: syncpoint {} stays at {}, the guest saw {}", id,
                                    syncpoints.GetGuestSyncpointValue(id), value);
                    }
                    break;
                }
                if (++spins < 64) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds{50});
                }
            }
        }
    }

    bool PlayStream(std::string& error) {
        auto& gpu = system.GPU();
        for (size_t i = stream_begin; i < recording.events.size(); ++i) {
            const Event& event = recording.events[i];
            if (const auto* submit = std::get_if<Submit>(&event)) {
                gpu.PushGPUEntries(submit->channel, ToCommandList(*submit));
            } else if (const auto* pages = std::get_if<PhysPages>(&event)) {
                WritePages(*pages, true);
            } else if (const auto* observe = std::get_if<SyncpointObserve>(&event)) {
                WaitObserved(*observe);
            } else if (std::holds_alternative<FrameBoundary>(event)) {
                gpu.GuestFrameBoundary();
            } else if (const auto* composite = std::get_if<Composite>(&event)) {
                std::vector<Tegra::FramebufferConfig> layers;
                for (const Layer& layer : composite->layers) {
                    layers.push_back(ToFramebuffer(layer));
                }
                std::vector<Service::Nvidia::NvFence> fences;
                for (const auto& [id, value] : composite->fences) {
                    fences.push_back({id, value});
                }
                gpu.RequestComposite(std::move(layers), std::move(fences));
                if (capture && recorded_shots.contains(composite->index)) {
                    // One screenshot at a time: the next composite waits for this one's.
                    WaitForComposites(++requested_composites);
                    std::unique_lock lock{shot_mutex};
                    shot_cv.wait_for(lock, std::chrono::seconds{5},
                                     [this] { return shots_pending == 0; });
                } else {
                    ++requested_composites;
                }
            } else if (const auto* shot = std::get_if<Screenshot>(&event)) {
                if (current_loop == 0) {
                    static_cast<void>(WriteFile(
                        options.out_dir / fmt::format("recorded-{:04}.png", shot->composite_index),
                        shot->png));
                    ++report.frames_compared;
                }
            } else if (const auto* increment = std::get_if<SyncpointIncrement>(&event)) {
                auto& syncpoints = system.Host1x().GetSyncpointManager();
                if (increment->guest) {
                    syncpoints.IncrementGuest(increment->id);
                }
                if (increment->host) {
                    syncpoints.IncrementHost(increment->id);
                }
            } else if (const auto* gmmu = std::get_if<GmmuOps>(&event)) {
                if (gmmu->ordered_id == 0) {
                    ApplyGmmu(gmmu->ops);
                } else if (gmmu->before_later_reads) {
                    gpu.MapInOrder(0, 0, [this, ops = gmmu->ops] { ApplyGmmu(ops); });
                } else {
                    gpu.RunInOrder([this, ops = gmmu->ops] { ApplyGmmu(ops); });
                }
            } else if (const auto* map = std::get_if<SmmuMap>(&event)) {
                MapSmmu(*map);
            } else if (const auto* unmap = std::get_if<SmmuUnmap>(&event)) {
                Smmu().Unmap(unmap->address, unmap->size);
            } else if (const auto* zero = std::get_if<ZeroReadRange>(&event)) {
                Smmu().AddZeroReadRange(zero->address, zero->size);
            } else if (const auto* space = std::get_if<AddressSpace>(&event)) {
                CreateSpace(*space);
            } else if (const auto* channel = std::get_if<Channel>(&event)) {
                if (!CreateChannel(*channel, error)) {
                    return false;
                }
            } else {
                error = "unexpected record in the stream";
                return false;
            }
        }
        return true;
    }

    void WaitForComposites(u64 count) {
        std::unique_lock lock{timing_mutex};
        timing_cv.wait_for(lock, DrainTimeout, [this, count] { return shown >= count; });
    }

    /// GPU thread, before composite `request` is shown.
    void OnComposite(u64 request) {
        const auto now = Clock::now();
        const u64 thread_ns = Common::CurrentThreadCpuTimeNs().value_or(0);
        const u64 index = composites_per_loop == 0 ? 0 : (request - 1) % composites_per_loop;
        {
            std::scoped_lock lock{timing_mutex};
            const bool first = request == 1 || index == 0;
            frames.push_back(FrameTiming{
                .loop = static_cast<u32>(
                    composites_per_loop == 0 ? 0 : (request - 1) / composites_per_loop),
                .index = index,
                .ms = first ? 0.0 : Ms(now - last_shown),
                .thread_ms =
                    first ? 0.0 : static_cast<double>(thread_ns - last_thread_ns) / 1'000'000.0,
            });
            last_shown = now;
            last_thread_ns = thread_ns;
            ++shown;
        }
        timing_cv.notify_all();
        if (!capture || !recorded_shots.contains(index) || shot_width == 0 || shot_height == 0) {
            return;
        }
        auto& renderer = system.GPU().Renderer();
        if (renderer.IsScreenshotPending()) {
            return;
        }
        auto pixels = std::make_shared<std::vector<u8>>(size_t{shot_width} * shot_height * 4);
        {
            std::scoped_lock lock{shot_mutex};
            ++shots_pending;
        }
        renderer.RequestScreenshot(
            pixels->data(),
            [this, pixels, index](bool invert_y) {
                const auto png = EncodeScreenshotPng(*pixels, shot_width, shot_height, invert_y);
                static_cast<void>(
                    WriteFile(options.out_dir / fmt::format("replayed-{:04}.png", index), png));
                {
                    std::scoped_lock lock{shot_mutex};
                    --shots_pending;
                }
                shot_cv.notify_all();
            },
            Layout::DefaultFrameLayout(shot_width, shot_height));
    }

    Core::System& system;
    Core::Frontend::EmuWindow& window;
    ReplayOptions options;
    ReplayReport report;
    RecordingFile recording;
    u64 program_id{};
    u32 shot_width{};
    u32 shot_height{};

    u8* physical_base{};
    u64 physical_pages{};
    Common::PageTable page_table;
    std::unique_ptr<Core::Memory::Memory> process_memory;
    Core::Asid asid{};
    bool asid_registered{};
    Common::ScratchBuffer<u32> scratch;

    std::map<u64, std::unique_ptr<Tegra::MemoryManager>> spaces;
    std::map<s32, std::shared_ptr<Tegra::Control::ChannelState>> channels;
    std::vector<s32> init_channels;
    std::vector<const PhysPages*> initial_pages;
    std::vector<SmmuRun> smmu_runs;
    const std::vector<u8>* shared_state{};
    std::vector<std::pair<s32, const std::vector<u8>*>> engine_states;
    const Syncpoints* initial_syncpoints{};
    std::unordered_map<u64, const Screenshot*> recorded_shots;
    size_t stream_begin{};
    bool stream_changes_state{};
    u64 composites_per_loop{};
    u64 requested_composites{};
    u32 current_loop{};
    bool capture{};
    u64 sync_timeouts{};

    std::mutex timing_mutex;
    std::condition_variable timing_cv;
    std::vector<FrameTiming> frames;
    Clock::time_point last_shown{};
    u64 last_thread_ns{};
    u64 shown{};

    std::mutex shot_mutex;
    std::condition_variable shot_cv;
    u32 shots_pending{};
};

} // Anonymous namespace

FrameStats SummarizeFrames(const std::vector<FrameTiming>& frames, u32 first_loop) {
    std::vector<double> times;
    for (const FrameTiming& frame : frames) {
        // The first composite of a loop has no interval before it.
        if (frame.loop >= first_loop && frame.index != 0) {
            times.push_back(frame.ms);
        }
    }
    FrameStats stats{};
    stats.frames = times.size();
    if (times.empty()) {
        return stats;
    }
    double sum = 0.0;
    for (const double t : times) {
        sum += t;
    }
    std::sort(times.begin(), times.end());
    const auto percentile = [&times](double p) {
        const size_t index = static_cast<size_t>(p * static_cast<double>(times.size() - 1) + 0.5);
        return times[std::min(index, times.size() - 1)];
    };
    stats.mean_ms = sum / static_cast<double>(times.size());
    stats.p50_ms = percentile(0.5);
    stats.p99_ms = percentile(0.99);
    stats.max_ms = times.back();
    stats.fps = stats.mean_ms > 0.0 ? 1000.0 / stats.mean_ms : 0.0;
    return stats;
}

ReplayReport RunReplay(Core::System& system, Core::Frontend::EmuWindow& window,
                       const ReplayOptions& options) {
    Replayer replayer{system, window, options};
    ReplayReport report = replayer.Run();
    // The GPU goes before the address spaces and channels the replayer made for it.
    if (system.IsPoweredOn()) {
        system.GPU().WaitForIdle();
        replayer.ReleaseProcess();
        system.ShutdownMainProcess();
    }
    static_cast<void>(WriteFile(options.out_dir / "report.json", [&] {
        const std::string json = ReportJson(report);
        return std::vector<u8>(json.begin(), json.end());
    }()));
    return report;
}

std::string ReportJson(const ReplayReport& report) {
    const u32 timed_loop = report.loop_ms.size() > 1 ? 1 : 0;
    const FrameStats stats = SummarizeFrames(report.frames, timed_loop);
    std::string out = "{\n";
    out += fmt::format("  \"ok\": {},\n", report.ok ? "true" : "false");
    out += fmt::format("  \"error\": {},\n", JsonString(report.error));
    out += fmt::format("  \"events\": {},\n  \"submits\": {},\n  \"composites\": {},\n",
                       report.events, report.submits, report.composites);
    out += fmt::format("  \"frames_compared\": {},\n  \"setup_ms\": {:.1f},\n",
                       report.frames_compared, report.setup_ms);
    out += "  \"loop_ms\": [";
    for (size_t i = 0; i < report.loop_ms.size(); ++i) {
        out += fmt::format("{}{:.2f}", i == 0 ? "" : ", ", report.loop_ms[i]);
    }
    out += "],\n";
    out += fmt::format("  \"timed_from_loop\": {},\n", timed_loop);
    out += fmt::format("  \"frame_ms\": {{\"frames\": {}, \"mean\": {:.3f}, \"p50\": {:.3f}, "
                       "\"p99\": {:.3f}, \"max\": {:.3f}, \"fps\": {:.1f}}},\n",
                       stats.frames, stats.mean_ms, stats.p50_ms, stats.p99_ms, stats.max_ms,
                       stats.fps);
    out += "  \"native_verify\": {";
    for (size_t i = 0; i < report.verify_checks.size(); ++i) {
        out += fmt::format("{}{}: {{\"checks\": {}, \"differences\": {}}}", i == 0 ? "" : ", ",
                           JsonString(report.verify_checks[i].first),
                           report.verify_checks[i].second, report.verify_differences[i].second);
    }
    out += "},\n";
    out += "  \"frames\": [";
    for (size_t i = 0; i < report.frames.size(); ++i) {
        const FrameTiming& frame = report.frames[i];
        out += fmt::format("{}\n    {{\"loop\": {}, \"index\": {}, \"ms\": {:.3f}, "
                           "\"composite_thread_ms\": {:.3f}}}",
                           i == 0 ? "" : ",", frame.loop, frame.index, frame.ms, frame.thread_ms);
    }
    out += "\n  ],\n";
    out += fmt::format("  \"metadata\": {}\n", JsonString(report.metadata));
    out += "}\n";
    return out;
}

} // namespace VideoCommon::GpuRecord
