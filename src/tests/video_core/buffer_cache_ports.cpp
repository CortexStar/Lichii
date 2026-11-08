// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <barrier>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
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
#include "video_core/buffer_cache/upload_order.h"
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
        ++usage_marks;
        last_usage = {offset, size};
    }
    std::span<u8> Mapped() {
        return host_mapped ? std::span{bytes} : std::span<u8>{};
    }
    u64 LastUsageTick() const {
        return tick;
    }
    /// A made-up device address: the GPU page table tests check where entries point.
    u64 DeviceAddress() const {
        return 0x7000'0000'0000ULL + CpuAddr();
    }

    std::vector<u8> bytes;
    u32 usage_marks{};
    std::pair<u64, u64> last_usage{};
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
    bool IsFree(u64) const {
        return true;
    }
    void WaitSubmittedTick(u64) {}
    VideoCommon::UploadOrder DecideUploadOrder(const Buffer&,
                                               std::span<VideoCommon::BufferCopy>) const {
        return {};
    }
    Staging DownloadStagingBuffer(u64 size, bool = false) {
        staging = std::make_unique<Buffer>(*this, 0, size + 128, false);
        ++allocations;
        return {staging.get(), 128, std::span{staging->bytes}.subspan(128)};
    }
    Staging UploadStagingBuffer(u64 size) {
        return DownloadStagingBuffer(size);
    }
    void FreeDeferredStagingBuffer(Staging&) {}
    u64 CurrentTick() const {
        return 1;
    }
    /// Buffer to buffer, as when a new buffer takes over the ones it overlaps.
    void CopyBuffer(Buffer& dst, Buffer& src, std::span<const VideoCommon::BufferCopy> copies,
                    bool) {
        for (const auto& copy : copies) {
            REQUIRE(copy.src_offset + copy.size <= src.bytes.size());
            REQUIRE(copy.dst_offset + copy.size <= dst.bytes.size());
            std::memcpy(dst.bytes.data() + copy.dst_offset, src.bytes.data() + copy.src_offset,
                        copy.size);
        }
    }
    void ClearBuffer(Buffer& buffer, u32 offset, size_t size, u32 value) {
        REQUIRE(offset + size <= buffer.bytes.size());
        for (size_t i = 0; i + sizeof(value) <= size; i += sizeof(value)) {
            std::memcpy(buffer.bytes.data() + offset + i, &value, sizeof(value));
        }
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
                    VideoCommon::UploadOrder) {
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
        for (auto& copy : pending) {
            copy();
        }
        pending.clear();
        ++finishes;
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
    void BindUniformBuffer(Buffer& buffer, u32 offset, u32 size) {
        if (bank_entry != nullptr) {
            *bank_entry = {.address = buffer.DeviceAddress() + offset, .size = size, .reserved = 0};
        }
    }
    void SetBankEntry(Shader::Backend::SPIRV::BankTableEntry* entry) {
        bank_entry = entry;
    }
    std::pair<std::span<u8>, u64> AllocateBankTable(u32 size) {
        bank_table.assign(size, 0xcd);
        return {bank_table, 0x4000'0000};
    }
    Shader::Backend::SPIRV::BankTableEntry* bank_entry{};
    std::vector<u8> bank_table;
    bool SupportsGpuPageTable() const {
        return true;
    }
    u64 NullPageAddress() const {
        return NULL_PAGE;
    }
    u64 SyncPageTable(size_t, VideoCommon::GpuPageTable& table, bool upload = true) {
        if (!upload) {
            return 0x5000'0000;
        }
        table.TakeUpdates(page_table_updates);
        ++page_table_syncs;
        return 0x5000'0000;
    }
    static constexpr u64 NULL_PAGE = 0x6000'0000;
    std::vector<VideoCommon::GpuPageTable::Update> page_table_updates;
    u32 page_table_syncs{};
    std::span<u8> BindMappedUniformBuffer(size_t, u32, u32 size) {
        stream.resize(size);
        if (bank_entry != nullptr) {
            *bank_entry = {.address = 0x3000'0000, .size = size, .reserved = 0};
        }
        return stream;
    }

    u32 alignment{256};
    u32 allocations{}, downloads{}, uploads{}, finishes{}, waits{};
    u32 host_read_barriers{};
    u64 upload_bytes{}, waited_tick{};
    std::unique_ptr<Buffer> staging;
    std::vector<u8> stream;
    std::vector<std::function<void()>> pending;
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
    /// A dispatch or copy wrote [address, address + size) of buffer `id` (Nx2NoteListWrite).
    static void ListWrite(Cache& cache, BufferId id, DAddr address, u64 size) {
        cache.Nx2NoteListWrite(id, address, size);
    }
    static void MarkWritten(Cache& cache, BufferId id, DAddr address, u32 size) {
        cache.MarkWrittenBuffer(id, address, size);
    }
    /// The guest CPU read GPU data from [address, address + size) (GetFlushArea).
    static void ReadByCpu(Cache& cache, DAddr address, u64 size) {
        cache.RollCpuReadRanges();
        cache.cpu_read_ranges.Add(address, size);
    }
    static void UseGpuMemory(Cache& cache, Tegra::MemoryManager& memory) {
        cache.gpu_memory = &memory;
    }
    static void NextFrame(Cache& cache) {
        cache.native_bank_ranges_this_frame.Clear();
    }
    static void MarkUsed(Cache& cache, DAddr address, u64 size) {
        cache.MarkUsed(address, size);
    }
    static u32 CpuMarks(Cache& cache) {
        return cache.memory_tracker.cpu_marks;
    }
    /// One draw's storage binding through the GPU page table: the update, then the bind.
    static const GpuPageTable& Resident(Cache& cache, BufferCacheChannelInfo& channel,
                                        const Binding& binding, bool written) {
        cache.channel_state = &channel;
        cache.maxwell3d = &channel.maxwell3d;
        cache.gpu_memory = &channel.gpu_memory;
        auto& residency = cache.residencies[0][0];
        cache.ResolveResidency(residency, binding);
        cache.MakeResident(residency, written, ~u64{0});
        REQUIRE(cache.PageTableAddress() == 0x5000'0000);
        return *cache.CurrentPageTable();
    }
    static size_t Runs(Cache& cache) {
        return cache.residencies[0][0].runs.size();
    }
    static void Delete(Cache& cache, BufferId id) {
        cache.DeleteBuffer(id);
    }
    static BufferId Find(Cache& cache, DAddr address, u32 size) {
        return cache.FindBuffer(address, size, false);
    }
    /// One draw's constant buffers through the bank table, vertex (0) and fragment (4) stages.
    static void BankDraw(Cache& cache, BufferCacheChannelInfo& channel) {
        cache.channel_state = &channel;
        cache.maxwell3d = &channel.maxwell3d;
        cache.gpu_memory = &channel.gpu_memory;
        cache.SetUniformsByAddress(0, true);
        cache.SetUniformsByAddress(4, true);
        cache.BeginBankTable(false);
        cache.BindHostGraphicsUniformBuffers(0);
        cache.BindHostGraphicsUniformBuffers(4);
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

namespace {

/// Makes `page` a command list page for one test (nx2.h keeps them for the session).
struct ListPage {
    explicit ListPage(DAddr page) {
        Tegra::Nx2ClearGpuWrittenPages();
        Tegra::Nx2MarkCommandListPage(page);
    }
    ~ListPage() {
        Tegra::Nx2ClearGpuWrittenPages();
    }
    ListPage(const ListPage&) = delete;
    ListPage& operator=(const ListPage&) = delete;
};

bool AllBytes(std::span<const u8> bytes, u8 value) {
    return std::ranges::all_of(bytes, [value](u8 byte) { return byte == value; });
}

} // namespace

TEST_CASE("Command lists on words no GPU work wrote read guest memory, not the page's copy",
          "[buffer_ports]") {
    Fixture f;
    const ListPage list_page{0x20000};
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x77);
    std::memset(f.bytes + 0x10000, 0x11, 0x1000);
    // The page's first list drained the split and flushed the page (FlushGpuWrittenCommands).
    f.cache->Nx2NoteListSynced(0x20000);
    // A dispatch writes bytes 0x800-0x900 of the page; the copy taken after it is on its way.
    f.cache->Nx2CollectListCopies(true);
    Access::ListWrite(*f.cache, id, 0x20800, 0x100);
    REQUIRE(f.cache->Nx2CopyListPages(f.runtime.CurrentTick()));
    f.cache->Nx2CollectListCopies(false);
    using Read = Cache::Nx2ListRead;
    std::array<u8, 16> list{};
    REQUIRE(f.cache->Nx2ReadListCopy(0x20100, list, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x207f0, list, true) == Read::Synced);
    // The GPU completes the copy. A list on the dispatch's words reads them from it; one that
    // starts before them reads its other words from guest memory.
    f.runtime.Finish();
    REQUIRE(f.cache->Nx2ReadListCopy(0x20800, list, true) == Read::Copied);
    REQUIRE(AllBytes(list, 0x77));
    std::array<u8, 32> spanning{};
    REQUIRE(f.cache->Nx2ReadListCopy(0x207f0, spanning, true) == Read::Copied);
    REQUIRE(AllBytes(std::span{spanning}.first(16), 0x11));
    REQUIRE(AllBytes(std::span{spanning}.last(16), 0x77));
}

TEST_CASE("Command list pages the front has not flushed read every word from the copy",
          "[buffer_ports]") {
    Fixture f;
    const ListPage list_page{0x20000};
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    std::ranges::fill(Access::Get(*f.cache, id).bytes, 0x77);
    std::memset(f.bytes + 0x10000, 0x11, 0x1000);
    // GPU work may have written the page before it became a command list page: only the first
    // write since is noted, and guest memory need not hold the rest.
    f.cache->Nx2CollectListCopies(true);
    Access::ListWrite(*f.cache, id, 0x20800, 0x100);
    REQUIRE(f.cache->Nx2CopyListPages(f.runtime.CurrentTick()));
    f.cache->Nx2CollectListCopies(false);
    f.runtime.Finish();
    std::array<u8, 32> list{};
    REQUIRE(f.cache->Nx2ReadListCopy(0x20100, list, true) == Cache::Nx2ListRead::Copied);
    REQUIRE(AllBytes(list, 0x77));
    REQUIRE(f.cache->Nx2ReadListCopy(0x207f0, list, true) == Cache::Nx2ListRead::Copied);
    REQUIRE(AllBytes(list, 0x77));
}

TEST_CASE("Host writes give command list words back to guest memory and retire the page's copy",
          "[buffer_ports]") {
    Fixture f;
    const ListPage list_page{0x20000};
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    f.cache->Nx2NoteListSynced(0x20000);
    f.cache->Nx2CollectListCopies(true);
    Access::ListWrite(*f.cache, id, 0x20800, 0x100);
    REQUIRE(f.cache->Nx2CopyListPages(f.runtime.CurrentTick()));
    f.cache->Nx2CollectListCopies(false);
    f.runtime.Finish();
    using Read = Cache::Nx2ListRead;
    std::array<u8, 16> list{};
    REQUIRE(f.cache->Nx2ReadListCopy(0x20840, list, true) == Read::Copied);
    // The CPU, an inline upload or a copy of bytes guest memory holds writes 0x800-0x840 whole,
    // and two bytes of each of the words at 0x880 and 0x884.
    f.cache->Nx2NoteListHostWrite(0x20800, 0x40);
    f.cache->Nx2NoteListHostWrite(0x20882, 4);
    REQUIRE(f.cache->Nx2ReadListCopy(0x20800, list, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x20830, list, true) == Read::Synced);
    // GPU-written words it did not write whole: the copy no longer holds the page.
    REQUIRE(f.cache->Nx2ReadListCopy(0x20840, list, true) == Read::Unknown);
    REQUIRE(f.cache->Nx2ReadListCopy(0x20880, list, true) == Read::Unknown);
}

TEST_CASE("A flushed command list page reads guest memory until GPU work writes it again",
          "[buffer_ports]") {
    Fixture f;
    const ListPage list_page{0x20000};
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    f.cache->Nx2CollectListCopies(true);
    Access::ListWrite(*f.cache, id, 0x20000, 0x1000);
    REQUIRE(f.cache->Nx2CopyListPages(f.runtime.CurrentTick()));
    using Read = Cache::Nx2ListRead;
    std::array<u8, 16> list{};
    REQUIRE(f.cache->Nx2ReadListCopy(0x20800, list, true) == Read::Copied);
    // The front drained the split and flushed the page (DmaPusher::FlushGpuWrittenCommands).
    f.cache->Nx2NoteListSynced(0x20000);
    REQUIRE(f.cache->Nx2ReadListCopy(0x20800, list, true) == Read::Synced);
    Access::ListWrite(*f.cache, id, 0x20900, 0x40);
    REQUIRE(f.cache->Nx2CopyListPages(f.runtime.CurrentTick()));
    f.cache->Nx2CollectListCopies(false);
    f.runtime.Finish();
    REQUIRE(f.cache->Nx2ReadListCopy(0x20800, list, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x20900, list, true) == Read::Copied);
}

TEST_CASE("Copy engine clears give command list words back to guest memory", "[buffer_ports]") {
    Fixture f;
    const ListPage list_page{0x20000};
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    Tegra::MemoryManager gpu_memory{f.system, f.device, 32, 1ULL << 31, 16};
    gpu_memory.BindRasterizer(&f.rasterizer);
    gpu_memory.Map(0x200000, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    Access::UseGpuMemory(*f.cache, gpu_memory);
    f.cache->Nx2NoteListSynced(0x20000);
    f.cache->Nx2CollectListCopies(true);
    Access::ListWrite(*f.cache, id, 0x20800, 0x100);
    REQUIRE(f.cache->Nx2CopyListPages(f.runtime.CurrentTick()));
    f.cache->Nx2CollectListCopies(false);
    f.runtime.Finish();
    // 16 words cleared (MaxwellDMA's remapped constant fill writes them to guest memory too).
    REQUIRE(f.cache->DMAClear(0x200800, 16, 0));
    REQUIRE(AllBytes(std::span{Access::Get(*f.cache, id).bytes}.subspan(0x800, 0x40), 0));
    using Read = Cache::Nx2ListRead;
    std::array<u8, 16> list{};
    REQUIRE(f.cache->Nx2ReadListCopy(0x20830, list, true) == Read::Synced);
    REQUIRE(f.cache->Nx2ReadListCopy(0x20840, list, true) == Read::Unknown);
}

TEST_CASE("Only GPU writes over pages the guest reads ask for downloads right behind them",
          "[buffer_ports]") {
    Fixture f;
    f.memory->EnableNativeTiling();
    Access::UseGpuMemory(*f.cache, *f.memory);
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    REQUIRE_FALSE(f.cache->TakeReadPrefetchQueued());
    Access::ReadByCpu(*f.cache, 0x21000, 0x1000);
    Access::MarkWritten(*f.cache, id, 0x24000, 0x100);
    REQUIRE_FALSE(f.cache->TakeReadPrefetchQueued());
    Access::MarkWritten(*f.cache, id, 0x21800, 0x100);
    REQUIRE(f.cache->TakeReadPrefetchQueued());
    REQUIRE_FALSE(f.cache->TakeReadPrefetchQueued());
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
    REQUIRE(data[0x2000] == 0x6b);
    REQUIRE(data[0x10122] == 0x6b);
    REQUIRE(data[0x10123] == 0);
}

TEST_CASE("Overlapping native uniform bank windows upload each byte once a frame",
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
    std::memset(f.bytes + 0x10000, 0x3c, 0x20000);
    Access::Uniform(*f.cache, channel, id, 0x20000, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x10000);
    // Windows at other starts within the frame upload only what no window covered yet.
    Access::Uniform(*f.cache, channel, id, 0x20100, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x10100);
    Access::Uniform(*f.cache, channel, id, 0x20080, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x10100);
    const auto& data = Access::Get(*f.cache, id).bytes;
    REQUIRE(data[0x100ff] == 0x3c);
    REQUIRE(data[0x10100] == 0);
    // Next frame, unchanged guest bytes are compared with the page shadows, not uploaded;
    // a changed granule uploads alone, also from a window at a new start.
    Access::NextFrame(*f.cache);
    f.bytes[0x10000 + 0x4321] = 0x99;
    Access::Uniform(*f.cache, channel, id, 0x20040, 0x10000);
    REQUIRE(f.runtime.upload_bytes == 0x10100 + 0x100);
    REQUIRE(data[0x4321] == 0x99);
}

TEST_CASE("Marking a use reaches only an existing buffer that holds the range", "[buffer_ports]") {
    Fixture f;
    Access::MarkUsed(*f.cache, 0x20000, 8);
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    const u32 before = Access::Get(*f.cache, id).usage_marks;
    Access::MarkUsed(*f.cache, 0x24010, 8);
    REQUIRE(Access::Get(*f.cache, id).usage_marks == before + 1);
    REQUIRE(Access::Get(*f.cache, id).last_usage == std::pair<u64, u64>{0x4010, 8});
    Access::MarkUsed(*f.cache, 0x2fffc, 8);
    Access::MarkUsed(*f.cache, 0x40000, 8);
    REQUIRE(Access::Get(*f.cache, id).usage_marks == before + 1);
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

TEST_CASE("Storage through the GPU page table maps each device run to its buffer",
          "[buffer_ports]") {
    Fixture f;
    // Two 64 KiB GPU pages backed by device memory that is not contiguous.
    constexpr GPUVAddr base = 0x200000;
    f.memory->Map(base, 0x20000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.memory->Map(base + 0x10000, 0x40000, 0x10000, Tegra::PTEKind::PITCH, true);
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    const VideoCommon::Binding binding{
        .device_addr = 0x20000, .gpu_addr = base + 0x800, .size = 0x18000, .buffer_id = {}};
    const auto& table = Access::Resident(*f.cache, channel, binding, true);
    REQUIRE(Access::Runs(*f.cache) == 2);
    constexpr u64 host = 0x7000'0000'0000ULL;
    // Each byte translates to where the buffer holding its device address keeps it.
    REQUIRE(table.Translate(base + 0x800) == host + 0x20800);
    REQUIRE(table.Translate(base + 0xffff) == host + 0x2ffff);
    REQUIRE(table.Translate(base + 0x10000) == host + 0x40000);
    REQUIRE(table.Translate(base + 0x187ff) == host + 0x487ff);
    // Pages outside the binding stay at the null page.
    REQUIRE(table.Translate(base + 0x19000) == Runtime::NULL_PAGE);
    // The same binding again changes no entry.
    const u32 syncs = f.runtime.page_table_syncs;
    Access::Resident(*f.cache, channel, binding, true);
    REQUIRE(f.runtime.page_table_syncs == syncs + 1);
    REQUIRE(f.runtime.page_table_updates.empty());
    // A buffer that goes away takes its entries with it.
    const VideoCommon::BufferId second = Access::Find(*f.cache, 0x40000, 0x8800);
    Access::Delete(*f.cache, second);
    REQUIRE(table.Translate(base + 0x10000) == Runtime::NULL_PAGE);
    REQUIRE(table.Translate(base + 0x800) == host + 0x20800);
    // The next draw makes it resident again in a new buffer.
    Access::Resident(*f.cache, channel, binding, false);
    REQUIRE(table.Translate(base + 0x10010) == host + 0x40010);
}

TEST_CASE("Constant buffers by device address fill the draw's bank table, not descriptors",
          "[buffer_ports]") {
    using Shader::Backend::SPIRV::BankTableEntry;
    Fixture f;
    Tegra::Control::ChannelState state{0};
    state.memory_manager = &*f.memory;
    state.payload.emplace(f.system, *f.memory, state);
    VideoCommon::BufferCacheChannelInfo channel{state};
    VideoCommon::UniformBufferSizes sizes{};
    sizes[0][2] = 0x100;  // small: streamed
    sizes[4][0] = 0x2000; // past the stream size: read in place
    channel.uniform_buffer_sizes = &sizes;
    channel.enabled_uniform_buffer_masks[0] = 1U << 2;
    channel.enabled_uniform_buffer_masks[4] = 1U << 0;
    const auto id = Access::Add(*f.cache, 0x20000, 0x10000);
    channel.uniform_buffers[0][2] = {.device_addr = 0x20100, .size = 0x100, .buffer_id = id};
    channel.uniform_buffers[4][0] = {.device_addr = 0x24000, .size = 0x2000, .buffer_id = id};
    Access::BankDraw(*f.cache, channel);
    REQUIRE(f.cache->BankTableAddress() == 0x4000'0000);
    // Vertex through fragment: five stages of 18 banks.
    REQUIRE(f.runtime.bank_table.size() == 5 * 18 * sizeof(BankTableEntry));
    const auto entry = [&](u32 stage, u32 index) {
        BankTableEntry value;
        std::memcpy(&value, f.runtime.bank_table.data() + (stage * 18 + index) * sizeof(value),
                    sizeof(value));
        return value;
    };
    REQUIRE(entry(0, 2).address == 0x3000'0000);
    REQUIRE(entry(0, 2).size == 0x100);
    REQUIRE(entry(4, 0).address == 0x7000'0000'0000ULL + 0x24000);
    REQUIRE(entry(4, 0).size == 0x2000);
    // Banks the stages don't bind read zero from the null page.
    REQUIRE(entry(0, 0).address == Runtime::NULL_PAGE);
    REQUIRE(entry(0, 0).size == 0);
    REQUIRE(entry(3, 17).size == 0);
    REQUIRE(f.runtime.bank_entry == nullptr);
}
