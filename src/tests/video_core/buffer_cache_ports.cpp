// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <barrier>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/page_table.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace {

struct Runtime;

struct Buffer : VideoCommon::BufferBase {
    Buffer(Runtime&, VideoCommon::NullBufferParams params) : BufferBase{params} {}
    Buffer(Runtime&, DAddr address, u32 size, bool mapped)
        : BufferBase{address, size}, bytes(size), host_mapped{mapped} {}

    void MarkUsage(u64 offset, u64 size) {
        REQUIRE(offset + size <= bytes.size());
    }
    std::span<u8> Mapped() {
        return host_mapped ? std::span{bytes} : std::span<u8>{};
    }
    u64 LastUsageTick() const {
        return tick;
    }

    std::vector<u8> bytes;
    bool host_mapped{};
    u64 tick{};
};

struct Staging {
    Buffer* buffer;
    u64 offset;
    std::span<u8> mapped_span;
};

struct Runtime {
    bool CanReportMemoryUsage() const {
        return false;
    }
    u64 GetDeviceLocalMemory() const {
        return 0;
    }
    u64 GetMaxStorageBufferRange() const {
        return 1ULL << 30;
    }
    u32 GetStorageBufferAlignment() const {
        return alignment;
    }
    u32 GetUniformBufferAlignment() const {
        return 1;
    }
    bool IsFree(u64 tick) const {
        return tick <= completed_tick;
    }
    u64 CurrentTick() const {
        return submitted_tick + 1;
    }
    void WaitSubmittedTick(u64) {}
    bool CanReorderUpload(const Buffer&, std::span<VideoCommon::BufferCopy>) const {
        return false;
    }
    Staging DownloadStagingBuffer(u64 size, bool = false) {
        // Kept until the end: read downloads hold theirs past the next request.
        stagings.push_back(std::make_unique<Buffer>(*this, 0, size + 128, false));
        ++allocations;
        Buffer* const staging = stagings.back().get();
        return {staging, 128, std::span{staging->bytes}.subspan(128)};
    }
    Staging UploadStagingBuffer(u64 size) {
        return DownloadStagingBuffer(size);
    }
    void CopyBuffer(Buffer* dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src.bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst->bytes.size());
            pending.push_back([dst, &src, copy] {
                std::memcpy(dst->bytes.data() + copy.dst_offset, src.bytes.data() + copy.src_offset,
                            copy.size);
            });
            ++downloads;
        }
    }
    void CopyBuffer(Buffer& dst, Buffer* src, std::span<const VideoCommon::BufferCopy> copies, bool,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src->bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst.bytes.size());
            std::memcpy(dst.bytes.data() + copy.dst_offset, src->bytes.data() + copy.src_offset,
                        copy.size);
            ++uploads;
            upload_bytes += copy.size;
        }
    }
    void Finish() {
        if (before_completion) {
            before_completion();
        }
        Complete(submitted_tick);
        for (auto& copy : pending) {
            copy();
        }
        pending.clear();
        ++finishes;
    }
    u64 SubmitForHostRead() {
        ++host_read_barriers;
        submitted.emplace_back(++submitted_tick, std::move(pending));
        pending.clear();
        return submitted_tick;
    }
    void WaitForHostRead(u64 tick) {
        if (before_completion) {
            before_completion();
        }
        Complete(tick);
        ++host_read_waits;
    }
    /// The host GPU runs what was submitted up to `tick`.
    void Complete(u64 tick) {
        while (!submitted.empty() && submitted.front().first <= tick) {
            for (auto& copy : submitted.front().second) {
                copy();
            }
            submitted.pop_front();
        }
        completed_tick = (std::max)(completed_tick, tick);
    }
    void Wait(u64 tick) {
        if (before_completion) {
            before_completion();
        }
        waited_tick = tick;
        ++waits;
    }
    void FinishForHostRead() {
        ++host_read_barriers;
        Finish();
    }
    void PreCopyBarrier() {}
    void PostCopyBarrier() {}
    void BindUniformBuffer(Buffer&, u32, u32) {}
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        stream.resize(size);
        return stream;
    }

    u32 alignment{256};
    u32 allocations{}, downloads{}, uploads{}, finishes{}, waits{};
    u32 host_read_barriers{}, host_read_waits{};
    u64 upload_bytes{}, waited_tick{};
    u64 submitted_tick{}, completed_tick{};
    std::vector<std::unique_ptr<Buffer>> stagings;
    std::vector<u8> stream;
    std::vector<std::function<void()>> pending;
    std::deque<std::pair<u64, std::vector<std::function<void()>>>> submitted;
    std::function<void()> before_completion;
};

struct Tracker : VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager> {
    using MemoryTrackerBase::MemoryTrackerBase;
    void MarkRegionAsCpuModified(DAddr address, u64 size) {
        ++cpu_marks;
        MemoryTrackerBase::MarkRegionAsCpuModified(address, size);
    }
    u32 cpu_marks{};
};

struct Policy {
    using Runtime = ::Runtime;
    using Buffer = ::Buffer;
    using Async_Buffer = Staging;
    using MemoryTracker = Tracker;
    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using Cache = VideoCommon::BufferCache<Policy>;

} // namespace

namespace VideoCommon {

struct BufferCacheTestAccess {
    static Binding Bind(Cache& cache, Tegra::MemoryManager& memory, GPUVAddr descriptor,
                        bool written = false, bool native = false, bool table = false) {
        cache.gpu_memory = &memory;
        return cache.StorageBufferBinding(descriptor, 0, written, native, table);
    }
    static BufferId Add(Cache& cache, DAddr address, u32 size, bool mapped = false) {
        const auto id = cache.slot_buffers.insert(cache.runtime, address, size, mapped);
        cache.Register(id);
        cache.memory_tracker.ForEachUploadRange(address, size, [](auto, auto) {});
        return id;
    }
    static Buffer& Get(Cache& cache, BufferId id) {
        return cache.slot_buffers[id];
    }
    static void Dirty(Cache& cache, DAddr address, u64 size) {
        cache.memory_tracker.MarkRegionAsGpuModified(address, size);
        cache.gpu_modified_ranges.Add(address, size);
        cache.uncommitted_gpu_modified_ranges.Add(address, size);
        cache.committed_gpu_modified_ranges.emplace_back().Add(address, size);
        cache.pending_download_ranges.emplace_back().Add(address, size);
    }
    static bool Pending(Cache& cache, DAddr address, u64 size) {
        if (cache.gpu_modified_ranges.Intersects(address, size) ||
            cache.uncommitted_gpu_modified_ranges.Intersects(address, size)) {
            return true;
        }
        for (const auto& ranges : cache.committed_gpu_modified_ranges) {
            if (ranges.Intersects(address, size))
                return true;
        }
        for (const auto& ranges : cache.pending_download_ranges) {
            if (ranges.Intersects(address, size))
                return true;
        }
        return false;
    }
    static void DrawWrite(Cache& cache, DAddr page) {
        std::scoped_lock lock{cache.nx2_list_mutex};
        auto& entry = cache.nx2_list_pages[page];
        entry.draw_written = true;
        ++entry.writes;
    }
    static void Download(Cache& cache, std::span<const std::pair<DAddr, u64>> ranges) {
        cache.DownloadRangesMemory(ranges);
    }
    static void Uniform(Cache& cache, BufferCacheChannelInfo& channel, BufferId id, DAddr address,
                        u32 size) {
        cache.channel_state = &channel;
        cache.maxwell3d = &channel.maxwell3d;
        cache.gpu_memory = &channel.gpu_memory;
        channel.uniform_buffers[0][0] = {.device_addr = address, .size = size, .buffer_id = id};
        cache.BindHostGraphicsUniformBuffer(0, 0, 0, true);
    }
    static void NextFrame(Cache& cache) {
        cache.native_indirect_uniforms_this_frame.clear();
    }
    static u32 CpuMarks(Cache& cache) {
        return cache.memory_tracker.cpu_marks;
    }
    static void MarkWritten(Cache& cache, BufferId id, DAddr address, u32 size) {
        cache.MarkWrittenBuffer(id, address, size);
    }
    static size_t ReadDownloads(Cache& cache) {
        return cache.read_downloads.size();
    }
};

} // namespace VideoCommon

namespace {

using Access = VideoCommon::BufferCacheTestAccess;

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device{physical};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    std::optional<Tegra::MemoryManager> memory;
    Runtime runtime;
    std::unique_ptr<Cache> cache;
    std::unique_ptr<Kernel::KProcess> process;
    Core::Asid asid{};
    u8* bytes{};

    Fixture() {
        system.Initialize();
        process = std::make_unique<Kernel::KProcess>(system.Kernel());
        auto& table = process->GetPageTable().GetImpl();
        table.Resize(24, 12);
        bytes = physical.GetPointerFromRaw<u8>(0x10000);
        std::memset(bytes, 0, 0x40000);
        for (u64 page = 0; page < 64; ++page) {
            table.entries.GetAndFault(0x10 + page)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(bytes) - 0x10000);
        }
        process->GetMemory().SetCurrentPageTable(*process);
        asid = device.RegisterProcess(&process->GetMemory());
        device.BindInterface(&rasterizer);
        device.Map(0x10000, 0x10000, 0x40000, asid);
        ResetMemory();
        cache = std::make_unique<Cache>(device, runtime);
    }
    ~Fixture() {
        cache.reset();
        memory.reset();
        device.Unmap(0x10000, 0x40000);
        device.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(process.get());
    }
    void ResetMemory() {
        memory.reset();
        memory.emplace(system, device, 32, 1ULL << 31, 16);
        memory->BindRasterizer(&rasterizer);
        memory->Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    }
    void Descriptor(GPUVAddr address, u64 size) {
        std::memcpy(bytes, &address, sizeof(address));
        std::memcpy(bytes + 8, &size, sizeof(size));
    }
    VideoCommon::Binding Bind(bool written = false, bool native = false, bool table = false) {
        return Access::Bind(*cache, *memory, 0x100000, written, native, table);
    }
};

/// Hands FinishRead to a buffer cache, as RasterizerVulkan does.
class ReadRasterizer final : public VideoCore::RasterizerInterface {
public:
    explicit ReadRasterizer(Cache& cache_) : cache{cache_} {}

    void FinishRead(DAddr addr, u64 size) override {
        cache.FinishReadDownloads(addr, size);
    }
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
        return {addr, addr + size, true};
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

private:
    Cache& cache;
};

} // namespace

TEST_CASE("Storage binding cache tracks map changes and descriptor dependencies",
          "[buffer_ports]") {
    Fixture f;
    constexpr GPUVAddr base = 0x200000;
    f.memory->Map(base, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.Descriptor(base + 0x123, 0x456);
    for (int i = 0; i < 3; ++i) {
        const auto binding = f.Bind(true);
        REQUIRE(binding.device_addr == 0x20100);
        REQUIRE(binding.gpu_addr == base + 0x100);
        REQUIRE(binding.size == 0x479);
        REQUIRE(binding.buffer_id == VideoCommon::BufferId{});
        REQUIRE(binding.segment_count == 0);
    }
    REQUIRE(f.Bind().size == 0xf00);
    f.Descriptor(base + 0x123, 0x1456);
    REQUIRE(f.Bind(true).size == 0x1479);
    f.runtime.alignment = 4096;
    REQUIRE(f.Bind(true).device_addr == 0x20000);
    REQUIRE(f.Bind(true).size == 0x1579);
    REQUIRE(f.Bind(true, true).size == 0x10000);
    f.memory->Map(base, 0x30000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(f.Bind(true).device_addr == 0x30000);
    f.memory->MapSparse(base, 0x10000, true);
    REQUIRE(f.Bind().size == 0);
    f.memory->Unmap(base, 0x10000);
    REQUIRE(f.Bind().size == 0);
    f.memory->Map(base, 0x20000, 0x2000, Tegra::PTEKind::PITCH, false);
    REQUIRE(f.Bind(true, true).size == 0x2000);
    f.memory->Map(base + 0x1000, 0x40000, 0x1000, Tegra::PTEKind::PITCH, false);
    REQUIRE(f.Bind(true, true).size == 0x1000);
    const auto old_owner = f.memory->GetID();
    f.ResetMemory();
    f.memory->Map(base, 0x40000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(f.memory->GetID() != old_owner);
    REQUIRE(f.Bind(true).device_addr == 0x40000);
    f.Descriptor(0, 0x123);
    REQUIRE(f.Bind().size == 0);
}

TEST_CASE("Storage binding cache preserves native and table size policies", "[buffer_ports]") {
    Fixture f;
    constexpr GPUVAddr base = 0x4000000;
    f.memory->Map(base, 0x10000000, 80 << 20, Tegra::PTEKind::PITCH, true);
    f.Descriptor(base, 256);
    REQUIRE(f.Bind().size == 4096);
    REQUIRE(f.Bind(false, true).size == 8 << 20);
    REQUIRE(f.Bind(false, false, true).size == 80 << 20);
    REQUIRE(f.Bind(true, false, true).size == 256);
    f.Descriptor(base, (1ULL << 32) | 256);
    REQUIRE(f.Bind().size == 8 << 20);
    REQUIRE(f.Bind(false, false, true).size == 80 << 20);
    f.Descriptor(base + (79 << 20), 0);
    REQUIRE(f.Bind(false, false, true).size == 1 << 20);
}

TEST_CASE("Storage binding cache does not reuse a destroyed address space", "[buffer_ports]") {
    Fixture f;
    f.memory->Map(0x200000, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.Descriptor(0x200000, 0x1000);
    REQUIRE(f.Bind(true).device_addr == 0x20000);
    const auto serial = f.memory->MapSerial();
    const auto owner = f.memory->GetID();
    const auto* address = &*f.memory;
    f.ResetMemory();
    f.memory->Map(0x200000, 0x30000, 0x10000, Tegra::PTEKind::PITCH, true);
    REQUIRE(&*f.memory == address);
    REQUIRE(f.memory->MapSerial() == serial);
    REQUIRE(f.memory->GetID() != owner);
    REQUIRE(f.Bind(true).device_addr == 0x30000);
}

TEST_CASE("Batch downloads clamp buffers, deduplicate pages and finish before publication",
          "[buffer_ports]") {
    Fixture f;
    const auto first = Access::Add(*f.cache, 0x20000, 0x10000);
    const auto second = Access::Add(*f.cache, 0x30000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, first).bytes, 0x12);
    std::ranges::fill(Access::Get(*f.cache, second).bytes, 0x34);
    Access::Dirty(*f.cache, 0x2f000, 0x2000);
    Access::Dirty(*f.cache, 0x32000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.bytes[0x1f000] == 0);
        REQUIRE(f.bytes[0x20000] == 0);
    };
    const std::array<std::pair<DAddr, u64>, 3> ranges{{
        {0x2f008, 0x1000},
        {0x2f000, 0x2000},
        {0x20000, 0},
    }};
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.allocations == 1);
    REQUIRE(f.runtime.downloads == 2);
    REQUIRE(f.bytes[0x1f000] == 0x12);
    REQUIRE(f.bytes[0x1ffff] == 0x12);
    REQUIRE(f.bytes[0x20000] == 0x34);
    REQUIRE(f.bytes[0x20fff] == 0x34);
    REQUIRE(f.bytes[0x22000] == 0);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x2f000, 0x2000));
    REQUIRE(Access::Pending(*f.cache, 0x32000, 0x1000));
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.finishes == 1);
}

TEST_CASE("Mapped and mixed batches make device writes host visible in one completion",
          "[buffer_ports]") {
    Fixture f;
    const auto first = Access::Add(*f.cache, 0x20000, 0x10000, true);
    const auto second = Access::Add(*f.cache, 0x30000, 0x10000, true);
    const auto clean = Access::Add(*f.cache, 0x40000, 0x10000, true);
    auto& a = Access::Get(*f.cache, first);
    auto& b = Access::Get(*f.cache, second);
    a.tick = 8;
    b.tick = 19;
    Access::Get(*f.cache, clean).tick = 99;
    const std::array<std::pair<DAddr, u64>, 3> ranges{{
        {0x20000, 0x1000},
        {0x30000, 0x1000},
        {0x40000, 0x1000},
    }};
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::Dirty(*f.cache, 0x30000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        REQUIRE(f.bytes[0x20000] == 0);
        std::ranges::fill(a.bytes, 0x56);
        std::ranges::fill(b.bytes, 0x78);
    };
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.host_read_barriers == 1);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.allocations == 0);
    REQUIRE(f.bytes[0x10000] == 0x56);
    REQUIRE(f.bytes[0x20000] == 0x78);
    b.host_mapped = false;
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::Dirty(*f.cache, 0x30000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 2);
        REQUIRE(f.bytes[0x10000] == 0x56);
        REQUIRE(f.bytes[0x20000] == 0x78);
        std::ranges::fill(a.bytes, 0x9a);
        std::ranges::fill(b.bytes, 0xbc);
    };
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.finishes == 2);
    REQUIRE(f.runtime.downloads == 1);
    REQUIRE(f.bytes[0x10000] == 0x9a);
    REQUIRE(f.bytes[0x20000] == 0xbc);
    Access::Download(*f.cache, ranges);
    REQUIRE(f.runtime.host_read_barriers == 2);
    REQUIRE(f.runtime.finishes == 2);
}

TEST_CASE("Single mapped download completes host visibility before guest publication",
          "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, true);
    auto& buffer = Access::Get(*f.cache, id);
    buffer.tick = 7;
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        std::ranges::fill(buffer.bytes, 0xcd);
    };
    f.cache->DownloadMemory(0x20000, 0x1000);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.waits == 0);
    REQUIRE(f.runtime.allocations == 0);
    REQUIRE(f.bytes[0x10000] == 0xcd);
    f.cache->DownloadMemory(0x20000, 0x1000);
    REQUIRE(f.runtime.finishes == 1);
}

TEST_CASE("Command page batch publishes only completed matching write generations",
          "[buffer_ports]") {
    Fixture f;
    const bool mapped = GENERATE(false, true);
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, mapped);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x42);
    Access::Dirty(*f.cache, 0x20000, 0x3000);
    Access::DrawWrite(*f.cache, 0x20000);
    Access::DrawWrite(*f.cache, 0x21000);
    Access::DrawWrite(*f.cache, 0x22000);
    std::array<u8, 4> out{};
    using Read = Cache::Nx2ListRead;
    f.runtime.before_completion = [&] {
        REQUIRE(f.runtime.host_read_barriers == 1);
        REQUIRE(f.bytes[0x10000] == 0);
        REQUIRE(f.cache->Nx2ReadListCopy(0x20000, out, true) == Read::Unknown);
        Access::DrawWrite(*f.cache, 0x22000);
    };
    const std::array<DAddr, 1> requested{0x20000};
    f.cache->Nx2FlushListPages(requested);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.downloads == (mapped ? 0U : 3U));
    REQUIRE(f.cache->Nx2ReadListCopy(0x20000, out, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x22000, out, true) == Read::Unknown);
    REQUIRE(f.bytes[0x12000] == 0x42);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, false) == Read::NeedsSync);
    Access::DrawWrite(*f.cache, 0x21000);
    REQUIRE(f.cache->Nx2ReadListCopy(0x21000, out, true) == Read::Unknown);
}

TEST_CASE("Command page readers remain ordered during batch completion", "[buffer_ports]") {
    struct {
        Core::DeviceMemory physical;
        Tegra::MaxwellDeviceMemoryManager device{physical};
        Runtime runtime;
        std::unique_ptr<Cache> cache = std::make_unique<Cache>(device, runtime);
    } f;
    Access::Add(*f.cache, 0x20000, 0x10000);
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    Access::DrawWrite(*f.cache, 0x20000);
    std::barrier rendezvous{2};
    Cache::Nx2ListRead before{}, after{};
    std::jthread reader{[&] {
        std::array<u8, 4> out{};
        rendezvous.arrive_and_wait();
        before = f.cache->Nx2ReadListCopy(0x20000, out, true);
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
        after = f.cache->Nx2ReadListCopy(0x20000, out, true);
    }};
    f.runtime.before_completion = [&] {
        rendezvous.arrive_and_wait();
        rendezvous.arrive_and_wait();
    };
    const std::array<DAddr, 1> requested{0x20000};
    f.cache->Nx2FlushListPages(requested);
    rendezvous.arrive_and_wait();
    reader.join();
    REQUIRE(before == Cache::Nx2ListRead::Unknown);
    REQUIRE(after == Cache::Nx2ListRead::Synced);
    REQUIRE(f.runtime.finishes == 1);
    REQUIRE(f.runtime.downloads == 1);
}

TEST_CASE("Native uniform banks upload directly without overwriting GPU-owned bytes",
          "[buffer_ports]") {
    Fixture f;
    f.memory->EnableNativeTiling();
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    VideoCommon::UniformBufferSizes sizes{};
    sizes[0][0] = 0x10000;
    channel.uniform_buffer_sizes = &sizes;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::memset(f.bytes + 0x10000, 0x5a, 0x10000);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x10000);
    REQUIRE(Access::CpuMarks(*f.cache) == 0);
    REQUIRE(Access::Get(*f.cache, id).bytes[0xffff] == 0x5a);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.uploads == 1);
    Access::NextFrame(*f.cache);
    Access::Dirty(*f.cache, 0x21000, 0x1000);
    std::memset(f.bytes + 0x10000, 0xa5, 0x10000);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.uploads == 1);
    REQUIRE(Access::Get(*f.cache, id).bytes[0x1000] == 0x5a);
}

TEST_CASE("Native uniform uploads stop at holes and keep unaligned bank boundaries",
          "[buffer_ports]") {
    Fixture f;
    f.memory->EnableNativeTiling();
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    VideoCommon::UniformBufferSizes sizes{};
    sizes[0][0] = 0x10000;
    channel.uniform_buffer_sizes = &sizes;
    const auto id = Access::Add(*f.cache, 0x20000, 0x20000);
    std::memset(f.bytes + 0x10000, 0x6b, 0x20000);
    f.device.Unmap(0x22000, 0x1000);
    Access::Uniform(*f.cache, channel, id, 0x20123, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x1edd);
    const auto& data = Access::Get(*f.cache, id).bytes;
    REQUIRE(data[0x122] == 0);
    REQUIRE(data[0x123] == 0x6b);
    REQUIRE(data[0x1fff] == 0x6b);
    REQUIRE(data[0x2000] == 0);
    REQUIRE(data[0x3000] == 0);
    REQUIRE(Access::CpuMarks(*f.cache) == 0);
    f.device.Map(0x22000, 0x22000, 0x1000, f.asid);
    Access::NextFrame(*f.cache);
    Access::Uniform(*f.cache, channel, id, 0x20123, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x11edd);
    REQUIRE(data[0x10122] == 0x6b);
    REQUIRE(data[0x10123] == 0);
}

TEST_CASE("Downloads keep CPU writes collected and not yet applied", "[buffer_ports]") {
    Fixture f;
    const auto staged = Access::Add(*f.cache, 0x20000, 0x10000);
    const auto mapped = Access::Add(*f.cache, 0x30000, 0x10000, true);
    std::ranges::fill(Access::Get(*f.cache, staged).bytes, 0x12);
    std::ranges::fill(Access::Get(*f.cache, mapped).bytes, 0x34);
    f.cache->SetPendingCpuWrites([](const std::function<void(DAddr, size_t)>& callback) {
        callback(0x20100, 0x10);
        callback(0x30ff8, 0x10);
    });
    std::memset(f.bytes + 0x10100, 0xee, 0x10);
    std::memset(f.bytes + 0x20ff8, 0xee, 0x10);
    Access::Dirty(*f.cache, 0x20000, 0x2000);
    Access::Dirty(*f.cache, 0x30000, 0x2000);
    const std::array<std::pair<DAddr, u64>, 1> ranges{{{0x20000, 0x2000}}};
    Access::Download(*f.cache, ranges);
    REQUIRE(f.bytes[0x100ff] == 0x12);
    REQUIRE(std::all_of(f.bytes + 0x10100, f.bytes + 0x10110, [](u8 b) { return b == 0xee; }));
    REQUIRE(f.bytes[0x10110] == 0x12);
    REQUIRE(f.bytes[0x11fff] == 0x12);
    f.cache->DownloadMemory(0x30000, 0x2000);
    REQUIRE(f.bytes[0x20ff7] == 0x34);
    REQUIRE(std::all_of(f.bytes + 0x20ff8, f.bytes + 0x21008, [](u8 b) { return b == 0xee; }));
    REQUIRE(f.bytes[0x21008] == 0x34);
}

TEST_CASE("Read downloads stay GPU-modified until the reading thread writes them",
          "[buffer_ports]") {
    Fixture f;
    const bool mapped = GENERATE(false, true);
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000, mapped);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x12);
    Access::Dirty(*f.cache, 0x20000, 0x2000);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    REQUIRE(f.runtime.finishes == 0);
    REQUIRE(f.runtime.downloads == 1);
    REQUIRE(f.runtime.host_read_barriers == 1);
    REQUIRE(VideoCore::g_read_downloads == 1);
    REQUIRE(f.bytes[0x10000] == 0);
    REQUIRE(Access::Pending(*f.cache, 0x20000, 0x1000));
    // Another read of the range waits for the same copy, and its flush copies nothing.
    const auto area = f.cache->GetFlushArea(0x20800, 4);
    REQUIRE(area);
    REQUIRE_FALSE(area->preemtive);
    REQUIRE(area->in_flight);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    REQUIRE(f.runtime.downloads == 1);
    REQUIRE(f.runtime.submitted_tick == 1);
    f.cache->FinishReadDownloads(0x20800, 4);
    REQUIRE(f.runtime.host_read_waits == 1);
    REQUIRE(VideoCore::g_read_downloads == 0);
    REQUIRE(f.bytes[0x10000] == 0x12);
    REQUIRE(f.bytes[0x10fff] == 0x12);
    REQUIRE(f.bytes[0x11000] == 0);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x1000));
    REQUIRE(Access::Pending(*f.cache, 0x21000, 0x1000));
    REQUIRE(f.cache->GetFlushArea(0x20800, 4)->preemtive);
    f.cache->FinishReadDownloads(0x20000, 0x2000);
    REQUIRE(f.runtime.host_read_waits == 1);
}

TEST_CASE("Read downloads keep CPU writes made while they were on their way", "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x12);
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    // After the submission: one write still collected, one already applied.
    std::memset(f.bytes + 0x10100, 0xee, 0x10);
    std::memset(f.bytes + 0x10200, 0xdd, 0x10);
    f.cache->SetPendingCpuWrites(
        [](const std::function<void(DAddr, size_t)>& callback) { callback(0x20100, 0x10); });
    const std::array<std::pair<DAddr, u64>, 1> applied{{{0x20200, 0x10}}};
    f.cache->ClaimCpuWrites(applied);
    f.cache->FinishReadDownloads(0x20000, 0x1000);
    REQUIRE(f.bytes[0x100ff] == 0x12);
    REQUIRE(std::all_of(f.bytes + 0x10100, f.bytes + 0x10110, [](u8 b) { return b == 0xee; }));
    REQUIRE(f.bytes[0x10110] == 0x12);
    REQUIRE(std::all_of(f.bytes + 0x10200, f.bytes + 0x10210, [](u8 b) { return b == 0xdd; }));
    REQUIRE(f.bytes[0x10210] == 0x12);
    REQUIRE(f.bytes[0x10fff] == 0x12);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x1000));
}

TEST_CASE("Read downloads land oldest first and leave newer GPU writes to download",
          "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    auto& buffer = Access::Get(*f.cache, id);
    std::ranges::fill(buffer.bytes, 0x12);
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    // The GPU thread's next work writes part of the range: on the host GPU, after the copy.
    f.runtime.pending.push_back(
        [&buffer] { std::fill_n(buffer.bytes.begin() + 0x800, 0x100, u8{0x34}); });
    Access::MarkWritten(*f.cache, id, 0x20800, 0x100);
    const auto area = f.cache->GetFlushArea(0x20800, 4);
    REQUIRE_FALSE(area->preemtive);
    REQUIRE_FALSE(area->in_flight);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    REQUIRE(f.runtime.downloads == 2);
    REQUIRE(Access::ReadDownloads(*f.cache) == 2);
    REQUIRE(f.cache->GetFlushArea(0x20800, 4)->in_flight);
    // The first copy holds what a synchronous flush would have left for the whole range.
    f.cache->FinishReadDownloads(0x20000, 4);
    REQUIRE(Access::ReadDownloads(*f.cache) == 1);
    REQUIRE(f.bytes[0x10000] == 0x12);
    REQUIRE(f.bytes[0x10800] == 0x12);
    REQUIRE(Access::Pending(*f.cache, 0x20800, 0x100));
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x800));
    f.cache->FinishReadDownloads(0x20800, 4);
    REQUIRE(f.bytes[0x107ff] == 0x12);
    REQUIRE(f.bytes[0x10800] == 0x34);
    REQUIRE(f.bytes[0x108ff] == 0x34);
    REQUIRE(f.bytes[0x10900] == 0x12);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x1000));
    // Both written in order even when the newer one's reader comes first.
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    f.runtime.pending.push_back(
        [&buffer] { std::fill_n(buffer.bytes.begin() + 0x800, 0x100, u8{0x56}); });
    Access::MarkWritten(*f.cache, id, 0x20800, 0x100);
    f.cache->DownloadMemoryForRead(0x20800, 0x100);
    REQUIRE(Access::ReadDownloads(*f.cache) == 2);
    f.cache->FinishReadDownloads(0x20800, 4);
    REQUIRE(Access::ReadDownloads(*f.cache) == 0);
    REQUIRE(f.bytes[0x10800] == 0x56);
    REQUIRE(f.bytes[0x10900] == 0x12);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x1000));
}

TEST_CASE("Read downloads land before a later fence's downloads", "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    auto& buffer = Access::Get(*f.cache, id);
    std::ranges::fill(buffer.bytes, 0x12);
    Access::MarkWritten(*f.cache, id, 0x20000, 0x1000);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    // A later GPU write the next fence downloads, and the host GPU runs all of it before anyone
    // waits for the read's copy.
    f.runtime.pending.push_back([&buffer] { std::fill_n(buffer.bytes.begin(), 0x100, u8{0x34}); });
    Access::MarkWritten(*f.cache, id, 0x20000, 0x100);
    f.cache->CommitAsyncFlushes();
    f.runtime.Finish();
    f.cache->PopAsyncFlushes();
    f.cache->FinishReadDownloads(0x20000, 0x1000);
    REQUIRE(f.bytes[0x10000] == 0x34);
    REQUIRE(f.bytes[0x100ff] == 0x34);
    REQUIRE(f.bytes[0x10100] == 0x12);
    REQUIRE(f.bytes[0x10fff] == 0x12);
    REQUIRE(Access::ReadDownloads(*f.cache) == 0);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x1000));
}

TEST_CASE("Preflushed ranges wait for the read download still on its way", "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x12);
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    // The first read marks the range for fence downloads, then flushes it.
    const auto first = f.cache->GetFlushArea(0x20000, 4);
    REQUIRE_FALSE(first->preemtive);
    REQUIRE_FALSE(first->in_flight);
    f.cache->DownloadMemoryForRead(first->start_address, first->end_address - first->start_address);
    const auto second = f.cache->GetFlushArea(0x20010, 4);
    REQUIRE_FALSE(second->preemtive);
    REQUIRE(second->in_flight);
    f.cache->FinishReadDownloads(second->start_address,
                                 second->end_address - second->start_address);
    REQUIRE(f.bytes[0x10010] == 0x12);
    REQUIRE(f.cache->GetFlushArea(0x20010, 4)->preemtive);
}

TEST_CASE("GPU writes to guest memory land the read downloads under them first", "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x12);
    ReadRasterizer reads{*f.cache};
    Tegra::MemoryManager gpu_memory{f.system, f.device, 32, 1ULL << 31, 16};
    gpu_memory.BindRasterizer(&reads);
    gpu_memory.Map(0x200000, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    Access::Dirty(*f.cache, 0x20000, 0x1000);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    // A semaphore release, then an inline upload, after the copy was taken.
    gpu_memory.Write<u32>(0x200100, 0xdeadbeef);
    REQUIRE(VideoCore::g_read_downloads == 0);
    const u32 words = 0x55555555;
    gpu_memory.WriteBlockUnsafe(0x200200, &words, sizeof(words));
    u32 value{};
    std::memcpy(&value, f.bytes + 0x10100, sizeof(value));
    REQUIRE(value == 0xdeadbeef);
    std::memcpy(&value, f.bytes + 0x10200, sizeof(value));
    REQUIRE(value == 0x55555555);
    REQUIRE(f.bytes[0x10000] == 0x12);
    REQUIRE(f.bytes[0x10104] == 0x12);
    REQUIRE(f.bytes[0x10fff] == 0x12);
}

TEST_CASE("Read downloads take again what a GPU write outside the cache's records changed",
          "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    auto& buffer = Access::Get(*f.cache, id);
    std::ranges::fill(buffer.bytes, 0x12);
    Access::MarkWritten(*f.cache, id, 0x20000, 0x100);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    // After the read's copy: a query result copy into the buffer (SyncValues), then a GPU write
    // elsewhere on the page, which the next read downloads.
    f.runtime.pending.push_back(
        [&buffer] { std::fill_n(buffer.bytes.begin() + 0x40, 0x10, u8{0x56}); });
    f.cache->OutdateReadDownloads(0x20040, 0x10);
    f.runtime.pending.push_back(
        [&buffer] { std::fill_n(buffer.bytes.begin() + 0x800, 0x10, u8{0x34}); });
    Access::MarkWritten(*f.cache, id, 0x20800, 0x10);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    f.cache->FinishReadDownloads(0x20000, 0x1000);
    REQUIRE(f.bytes[0x1003f] == 0x12);
    REQUIRE(f.bytes[0x10040] == 0x56);
    REQUIRE(f.bytes[0x1004f] == 0x56);
    REQUIRE(f.bytes[0x10050] == 0x12);
    REQUIRE(f.bytes[0x10800] == 0x34);
    REQUIRE_FALSE(Access::Pending(*f.cache, 0x20000, 0x1000));
}

TEST_CASE("Switch 2 writes to guest memory land read downloads only on GPU-written pages",
          "[buffer_ports]") {
    Fixture f;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x12);
    ReadRasterizer reads{*f.cache};
    Tegra::MemoryManager gpu_memory{f.system, f.device, 32, 1ULL << 31, 16};
    gpu_memory.BindRasterizer(&reads);
    gpu_memory.Map(0x200000, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    struct Nx2Title {
        Nx2Title() {
            Tegra::g_nx2_ampere = true;
        }
        ~Nx2Title() {
            Tegra::g_nx2_ampere = false;
            Tegra::Nx2ClearGpuWrittenPages();
        }
    } nx2_title;
    Access::MarkWritten(*f.cache, id, 0x20000, 0x1000);
    f.cache->DownloadMemoryForRead(0x20000, 0x1000);
    // A page no GPU write reached: no download can be on its way there.
    gpu_memory.Write<u32>(0x201000, 0xdeadbeef);
    REQUIRE(VideoCore::g_read_downloads == 1);
    REQUIRE(f.runtime.host_read_waits == 0);
    gpu_memory.Write<u32>(0x200100, 0xcafef00d);
    REQUIRE(VideoCore::g_read_downloads == 0);
    REQUIRE(f.runtime.host_read_waits == 1);
    u32 value{};
    std::memcpy(&value, f.bytes + 0x10100, sizeof(value));
    REQUIRE(value == 0xcafef00d);
    std::memcpy(&value, f.bytes + 0x11000, sizeof(value));
    REQUIRE(value == 0xdeadbeef);
    REQUIRE(f.bytes[0x10000] == 0x12);
    REQUIRE(f.bytes[0x10fff] == 0x12);
}
