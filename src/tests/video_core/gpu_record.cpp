// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <exception>
#include <filesystem>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_record/engine_state.h"
#include "video_core/gpu_record/record_format.h"
#include "video_core/gpu_record/replayer.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace GpuRecord = VideoCommon::GpuRecord;

namespace {

class NullRasterizer final : public VideoCore::RasterizerInterface {
public:
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&&) override {}
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}
};

GpuRecord::Event RoundTrip(const GpuRecord::Event& event) {
    const auto payload = GpuRecord::EncodeEvent(event);
    auto decoded = GpuRecord::DecodeEvent(GpuRecord::TypeOf(event), payload);
    REQUIRE(decoded.has_value());
    REQUIRE(GpuRecord::TypeOf(*decoded) == GpuRecord::TypeOf(event));
    // Same bytes again: every field came back.
    REQUIRE(GpuRecord::EncodeEvent(*decoded) == payload);
    return std::move(*decoded);
}

std::vector<GpuRecord::Event> SampleEvents() {
    using namespace GpuRecord;
    std::vector<Event> events;
    PhysPages pages;
    pages.pages = {7, 9};
    pages.data.resize(2 * PageSize);
    for (size_t i = 0; i < pages.data.size(); ++i) {
        pages.data[i] = static_cast<u8>(i * 31);
    }
    events.emplace_back(pages);
    events.emplace_back(SmmuMap{{{0x100, 7, 1}, {0x200, 9, 4}}});
    events.emplace_back(SmmuUnmap{0x200000, 0x4000});
    events.emplace_back(ZeroReadRange{0x300000, 0x10000});
    events.emplace_back(AddressSpace{3, 40, u64{1} << 34, 16, 12, true, 0xdead000});
    events.emplace_back(GmmuOps{5,
                                true,
                                {{GmmuOpKind::Map, true, 0xfe, 3, 0x10000, 0x200000, 0x20000},
                                 {GmmuOpKind::Unmap, false, 0, 3, 0x40000, 0, 0x1000}}});
    events.emplace_back(Channel{2, 3, 0x0400486024DEC000ULL});
    events.emplace_back(EngineState{2, {1, 2, 3, 4}});
    events.emplace_back(MacroContext{{9, 8, 7}});
    events.emplace_back(Syncpoints{{1, 2, 3}, {1, 2, 2}});
    events.emplace_back(SyncpointObserve{{{4, 100}, {7, 3}}});
    events.emplace_back(SyncpointIncrement{12, true, false});
    events.emplace_back(
        Submit{1, {0x123456789abcdefULL}, {0x20000001}, {0x55}, {1, 2, 3}, {0}, {}, 6});
    events.emplace_back(FrameBoundary{42});
    events.emplace_back(
        Composite{3,
                  {{0x8000000, 0, 1920, 1080, 1920, 1, 0, 0, 0, 1920, 1080, 0, true, 1, 0, 80}},
                  {{5, 77}}});
    events.emplace_back(Screenshot{3, 16, 8, {0x89, 'P', 'N', 'G'}});
    events.emplace_back(InitEnd{});
    return events;
}

} // namespace

TEST_CASE("GPU recording events encode and decode field by field", "[gpu_record]") {
    for (const auto& event : SampleEvents()) {
        static_cast<void>(RoundTrip(event));
    }
    const auto submit = std::get<GpuRecord::Submit>(
        RoundTrip(GpuRecord::Submit{4, {1, 2}, {}, {}, {10, 11, 12}, {0, 2}, {1}, 9}));
    REQUIRE(submit.channel == 4);
    REQUIRE(submit.submitted_words == std::vector<u32>{10, 11, 12});
    REQUIRE(submit.deferred_entries == std::vector<u32>{1});
    REQUIRE(submit.snapshot_suspensions == 9);
}

TEST_CASE("GPU recording payloads that end early or run on are refused", "[gpu_record]") {
    for (const auto& event : SampleEvents()) {
        auto payload = GpuRecord::EncodeEvent(event);
        payload.push_back(0);
        REQUIRE_FALSE(GpuRecord::DecodeEvent(GpuRecord::TypeOf(event), payload).has_value());
        payload.pop_back();
        if (!payload.empty()) {
            payload.pop_back();
            REQUIRE_FALSE(GpuRecord::DecodeEvent(GpuRecord::TypeOf(event), payload).has_value());
        }
    }
    // A vector claiming more elements than bytes left.
    GpuRecord::ByteWriter writer;
    writer.U64(u64{1} << 40);
    REQUIRE_FALSE(
        GpuRecord::DecodeEvent(GpuRecord::EventType::PhysPages, writer.bytes).has_value());
}

TEST_CASE("GPU recording files keep header and events, compressed or not", "[gpu_record]") {
    const auto path = std::filesystem::temp_directory_path() / "gpu_record_test.lxgpurec";
    GpuRecord::Header header{};
    header.metadata = "program_id=0400486024DEC000\nswitch2=1\nscreenshot_width=1280\n";
    const auto events = SampleEvents();
    {
        GpuRecord::FileWriter writer;
        REQUIRE(writer.Open(path, header));
        // A large page record goes through zstd.
        GpuRecord::PhysPages big;
        for (u32 page = 0; page < 64; ++page) {
            big.pages.push_back(page);
        }
        big.data.assign(64 * GpuRecord::PageSize, 0x5a);
        REQUIRE(writer.Write(GpuRecord::Event{big}));
        for (const auto& event : events) {
            REQUIRE(writer.Write(event));
        }
        REQUIRE(writer.Close());
    }
    std::string error;
    const auto read = GpuRecord::ReadFile(path, error);
    REQUIRE(read.has_value());
    REQUIRE(read->header.metadata == header.metadata);
    REQUIRE(read->events.size() == events.size() + 1);
    const auto& big = std::get<GpuRecord::PhysPages>(read->events.front());
    REQUIRE(big.pages.size() == 64);
    REQUIRE(big.data == std::vector<u8>(64 * GpuRecord::PageSize, 0x5a));
    for (size_t i = 0; i < events.size(); ++i) {
        REQUIRE(GpuRecord::EncodeEvent(read->events[i + 1]) == GpuRecord::EncodeEvent(events[i]));
    }
    REQUIRE(GpuRecord::MetadataValue(read->header.metadata, "switch2") == "1");
    REQUIRE(GpuRecord::MetadataValue(read->header.metadata, "screenshot_width") == "1280");
    REQUIRE(GpuRecord::MetadataValue(read->header.metadata, "screenshot").empty());

    // Cut short: refused with a reason instead of a partial stream.
    const auto size = std::filesystem::file_size(path);
    std::filesystem::resize_file(path, size - 3);
    REQUIRE_FALSE(GpuRecord::ReadFile(path, error).has_value());
    REQUIRE_FALSE(error.empty());
    std::filesystem::remove(path);
}

TEST_CASE("GPU address space snapshots list each mapped run once", "[gpu_record]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    NullRasterizer rasterizer;
    Tegra::MemoryManager memory{system, device_memory, 40, 1ULL << 34, 16};
    memory.BindRasterizer(&rasterizer);

    // Small pages: two adjacent maps that continue each other merge, a different kind splits.
    memory.Map(0x100000, 0x10000000, 0x2000, Tegra::PTEKind::PITCH, false);
    memory.Map(0x102000, 0x10002000, 0x1000, Tegra::PTEKind::PITCH, false);
    memory.Map(0x103000, 0x10003000, 0x1000, Tegra::PTEKind::GENERIC_16BX2, false);
    // Not contiguous in device memory: its own run.
    memory.Map(0x104000, 0x20000000, 0x1000, Tegra::PTEKind::PITCH, false);
    // Big pages and a sparse range.
    memory.Map(0x40000000, 0x30000000, 0x20000, Tegra::PTEKind::PITCH, true);
    memory.MapSparse(0x50000000, 0x30000, true);

    struct Run {
        GPUVAddr gpu;
        DAddr dev;
        u64 size;
        Tegra::PTEKind kind;
        bool big;
        bool reserved;
        bool operator==(const Run&) const = default;
    };
    std::vector<Run> runs;
    memory.ForEachMapping(
        [&](GPUVAddr gpu, DAddr dev, u64 size, Tegra::PTEKind kind, bool big, bool reserved) {
            runs.push_back({gpu, dev, size, kind, big, reserved});
        });
    const std::vector<Run> expected{
        {0x100000, 0x10000000, 0x3000, Tegra::PTEKind::PITCH, false, false},
        {0x103000, 0x10003000, 0x1000, Tegra::PTEKind::GENERIC_16BX2, false, false},
        {0x104000, 0x20000000, 0x1000, Tegra::PTEKind::PITCH, false, false},
        {0x40000000, 0x30000000, 0x20000, Tegra::PTEKind::PITCH, true, false},
        {0x50000000, 0, 0x30000, Tegra::PTEKind::INVALID, true, true},
    };
    REQUIRE(runs == expected);

    // Replaying the runs into a fresh address space gives the same snapshot.
    Tegra::MemoryManager copy{system, device_memory, 40, 1ULL << 34, 16};
    copy.BindRasterizer(&rasterizer);
    for (const Run& run : runs) {
        if (run.reserved) {
            copy.MapSparse(run.gpu, run.size, run.big);
        } else {
            copy.Map(run.gpu, run.dev, run.size, run.kind, run.big);
        }
    }
    std::vector<Run> copied;
    copy.ForEachMapping(
        [&](GPUVAddr gpu, DAddr dev, u64 size, Tegra::PTEKind kind, bool big, bool reserved) {
            copied.push_back({gpu, dev, size, kind, big, reserved});
        });
    REQUIRE(copied == runs);

    memory.Unmap(0x100000, 0x5000);
    runs.clear();
    memory.ForEachMapping(
        [&](GPUVAddr gpu, DAddr dev, u64 size, Tegra::PTEKind kind, bool big, bool reserved) {
            runs.push_back({gpu, dev, size, kind, big, reserved});
        });
    REQUIRE(runs.size() == 2);
}

TEST_CASE("A channel's captured engine state restores into a fresh channel", "[gpu_record]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    const bool ampere = GENERATE(false, true);
    Tegra::g_nx2_ampere = ampere;
    Tegra::MemoryManager memory{system, device_memory, 40, 1ULL << 34, 16};

    Tegra::Control::ChannelState source{1};
    source.memory_manager = &memory;
    source.Init(system, 0x0400486024DEC000ULL);
    std::mt19937 rng{1234};
    auto& m = source.payload->maxwell_3d;
    for (auto& value : m.regs.reg_array) {
        value = static_cast<u32>(rng());
    }
    for (auto& value : m.shadow_state.reg_array) {
        value = static_cast<u32>(rng());
    }
    m.state.shader_stages[2].const_buffers[5] = {0x123450000ULL, 0x1000, true};
    m.root_tables.tables[3][17] = 0xabcdef;
    m.root_tables.selected = 3;
    m.root_tables.loaded = true;
    m.GetNativeDataRam()[0x40] = 77;
    auto& c = source.payload->kepler_compute;
    for (auto& value : c.regs.reg_array) {
        value = static_cast<u32>(rng());
    }
    source.payload->fermi_2d.regs.reg_array[3] = 99;
    source.payload->kepler_memory.regs.reg_array[0x10] = 97;
    // A game binding the 2D engine on subchannel 6.
    source.payload->dma_pusher.BindSubchannel(&source.payload->fermi_2d, 6,
                                              Tegra::Engines::EngineTypes::Fermi2D);
    const auto blob = GpuRecord::CaptureChannel(source);
    const auto shared = GpuRecord::CaptureSharedState(source);

    Tegra::Control::ChannelState target{1};
    target.memory_manager = &memory;
    target.Init(system, 0x0400486024DEC000ULL);
    target.payload->maxwell_3d.dirty.flags.reset();
    std::string error;
    REQUIRE(GpuRecord::RestoreChannel(target, blob, error));
    REQUIRE(GpuRecord::RestoreSharedState(target, shared, error));
    // Captured again, the restored channel gives the same bytes.
    REQUIRE(GpuRecord::CaptureChannel(target) == blob);
    REQUIRE(GpuRecord::CaptureSharedState(target) == shared);
    REQUIRE(target.payload->maxwell_3d.regs.reg_array == m.regs.reg_array);
    REQUIRE(target.payload->maxwell_3d.state.shader_stages[2].const_buffers[5].address ==
            0x123450000ULL);
    REQUIRE(target.payload->maxwell_3d.root_tables.tables[3][17] == 0xabcdef);
    REQUIRE(target.payload->maxwell_3d.dirty.flags.all());

    // A blob from another build's engines (wrong size) is refused, not half applied.
    auto truncated = blob;
    truncated.resize(truncated.size() / 2);
    REQUIRE_FALSE(GpuRecord::RestoreChannel(target, truncated, error));
    REQUIRE_FALSE(error.empty());
    Tegra::g_nx2_ampere = false;
}

TEST_CASE("Replay frame statistics leave out each loop's first composite", "[gpu_record]") {
    std::vector<GpuRecord::FrameTiming> frames;
    for (u32 loop = 0; loop < 2; ++loop) {
        for (u64 index = 0; index < 5; ++index) {
            // Loop 0 is slow (cold caches); loop 1 4 ms a frame with one 8 ms frame.
            const double ms = index == 0 ? 0.0 : loop == 0 ? 50.0 : index == 3 ? 8.0 : 4.0;
            frames.push_back({loop, index, ms, ms / 2});
        }
    }
    const auto all = GpuRecord::SummarizeFrames(frames, 0);
    REQUIRE(all.frames == 8);
    const auto warm = GpuRecord::SummarizeFrames(frames, 1);
    REQUIRE(warm.frames == 4);
    REQUIRE(warm.mean_ms == 5.0);
    REQUIRE(warm.p50_ms == 4.0);
    REQUIRE(warm.max_ms == 8.0);
    REQUIRE(warm.fps == 200.0);
    REQUIRE(GpuRecord::SummarizeFrames({}, 0).frames == 0);
}
