// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "core/device_memory.h"
#include "video_core/nx2_debug_control.h"
#include "core/core.h"
#include "tests/core/system_test_context.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "shader_recompiler/exception.h"
#include "video_core/shader_cache.h"

namespace VideoCommon {
struct ShaderCacheTestAccess {
    static std::unique_ptr<ShaderCache> Create(Tegra::MaxwellDeviceMemoryManager& memory) {
        return std::unique_ptr<ShaderCache>{new ShaderCache{memory}};
    }
    static void Register(ShaderCache& cache, VAddr address, size_t size, u64 hash) {
        auto data = std::make_unique<ShaderInfo>();
        data->unique_hash = hash;
        cache.Register(std::move(data), address, size);
    }
    static const ShaderInfo* Find(const ShaderCache& cache, VAddr address) {
        return cache.TryGet(address);
    }
    static void RegisterNative(ShaderCache& cache, VAddr address, u64 hash, u64 key) {
        auto data = std::make_unique<ShaderInfo>();
        data->unique_hash = hash;
        auto lowered = std::make_shared<LoweredNativeCode>();
        lowered->native_program_key = key;
        data->lowered_native = std::move(lowered);
        cache.Register(std::move(data), address, 0x100);
    }
    static void Reject(ShaderCache& cache, VAddr address) {
        cache.rejected_native.emplace(address, std::make_exception_ptr(std::runtime_error{"rejected"}));
    }
    static bool IsRejected(ShaderCache& cache, VAddr address) {
        return cache.IsRejected(address);
    }
    static void Rethrow(ShaderCache& cache, VAddr address) {
        cache.RethrowIfRejected(address);
    }
    static void RememberRejection(ShaderCache& cache, VAddr address) {
        try {
            throw std::runtime_error{"lowering failed"};
        } catch (...) {
            cache.RememberRejection(address);
        }
    }
    static void ConfigureCompute(ShaderCache& cache, Tegra::Engines::KeplerCompute& compute,
                                 Tegra::MemoryManager& memory) {
        cache.kepler_compute = &compute;
        cache.gpu_memory = &memory;
    }
    static const ShaderInfo* Compute(ShaderCache& cache) {
        return cache.ComputeShader();
    }
    static void RejectMatrix(ShaderCache& cache, VAddr address) {
        cache.rejected_native.emplace(address, std::make_exception_ptr(
            Shader::RuntimeError{"native matrix multiply (not emulated on this host)"}));
    }
};
} // namespace VideoCommon

using VideoCommon::ShaderCacheTestAccess;

TEST_CASE("Shader batch invalidation matches sequential cache contents", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto batched = ShaderCacheTestAccess::Create(device_memory);
    auto sequential = ShaderCacheTestAccess::Create(device_memory);
    std::mt19937 random{0x16'67};
    constexpr VAddr base = 0x100000;
    for (unsigned iteration = 0; iteration < 600; ++iteration) {
        for (unsigned slot = 0; slot < 128; ++slot) {
            const auto address = base + slot * 4096;
            if (!ShaderCacheTestAccess::Find(*batched, address)) {
                const auto size = 1 + random() % 65536;
                ShaderCacheTestAccess::Register(*batched, address, size, iteration + slot);
                ShaderCacheTestAccess::Register(*sequential, address, size, iteration + slot);
            }
        }
        std::vector<std::pair<DAddr, size_t>> writes;
        for (unsigned i = 0, count = random() % 24; i < count; ++i) {
            writes.emplace_back(base + random() % (128 * 4096), random() % 32768);
            if (i % 4 == 0) {
                writes.push_back(writes.back());
            }
        }
        for (const auto& [address, size] : writes) {
            sequential->InvalidateRegion(address, size);
        }
        batched->InvalidateRegions(writes);
        for (unsigned slot = 0; slot < 128; ++slot) {
            const auto address = base + slot * 4096;
            const auto actual = ShaderCacheTestAccess::Find(*batched, address);
            const auto expected = ShaderCacheTestAccess::Find(*sequential, address);
            REQUIRE(bool(actual) == bool(expected));
            if (actual) {
                REQUIRE(actual->unique_hash == expected->unique_hash);
            }
        }
    }
}

TEST_CASE("Shader empty batches preserve pending invalidations", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr address = 0x100000;
    ShaderCacheTestAccess::Register(*cache, address, 32768, 123);
    cache->OnCacheInvalidation(address, 1);
    cache->InvalidateRegions({});
    REQUIRE(ShaderCacheTestAccess::Find(*cache, address));
    const std::array unrelated{std::pair<DAddr, size_t>{address + 0x100000, 4096}};
    cache->InvalidateRegions(unrelated);
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, address));
}

TEST_CASE("Debug server rejections drop matching native programs", "[shader_invalidation]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr by_key = 0x100000;
    constexpr VAddr by_hash = 0x200000;
    constexpr VAddr kept = 0x300000;
    constexpr VAddr maxwell = 0x400000;
    constexpr VAddr earlier = 0x500000;
    ShaderCacheTestAccess::RegisterNative(*cache, by_key, 0x11, 0xaaaa);
    ShaderCacheTestAccess::RegisterNative(*cache, by_hash, 0x22, 0xbbbb);
    ShaderCacheTestAccess::RegisterNative(*cache, kept, 0x33, 0xcccc);
    ShaderCacheTestAccess::Register(*cache, maxwell, 0x100, 0xaaaa);
    ShaderCacheTestAccess::Reject(*cache, earlier);

    VideoCore::Nx2Debug::BeginSession();
    const std::array<u64, 2> rejected{0xaaaa, 0x22};
    const u64 generation = VideoCore::Nx2Debug::SetRejected(rejected, true);
    const auto change = VideoCore::Nx2Debug::TakeRejectChange();
    REQUIRE(change);
    REQUIRE(change->generation == generation);
    REQUIRE_FALSE(change->unrejected);
    cache->RefreshDebugRejections(change->unrejected);
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, by_key));
    REQUIRE_FALSE(ShaderCacheTestAccess::Find(*cache, by_hash));
    REQUIRE(ShaderCacheTestAccess::Find(*cache, kept));
    // Only lowered native programs are matched; a Maxwell program with the same hash stays.
    REQUIRE(ShaderCacheTestAccess::Find(*cache, maxwell));
    REQUIRE(ShaderCacheTestAccess::IsRejected(*cache, earlier));

    const std::array<u64, 1> restored{0xaaaa};
    VideoCore::Nx2Debug::SetRejected(restored, false);
    const auto undo = VideoCore::Nx2Debug::TakeRejectChange();
    REQUIRE(undo);
    REQUIRE(undo->unrejected);
    cache->RefreshDebugRejections(undo->unrejected);
    REQUIRE_FALSE(ShaderCacheTestAccess::IsRejected(*cache, earlier));
    REQUIRE(VideoCore::Nx2Debug::RejectedHashes() == std::vector<u64>{0x22});
    VideoCore::Nx2Debug::EndSession();
}

TEST_CASE("A remembered rejection marks its page so filtered writes still clear it",
          "[shader_invalidation][page_filter]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr rejected = 0x7'1234'5100;
    REQUIRE_FALSE(cache->page_filter.Any(rejected, 1));

    ShaderCacheTestAccess::RememberRejection(*cache, rejected);
    REQUIRE(ShaderCacheTestAccess::IsRejected(*cache, rejected));
    CHECK(cache->page_filter.Any(rejected, 1));
    constexpr VAddr page = rejected & ~VAddr{0xfff};
    CHECK(cache->page_filter.Any(page, 0x1000));
    CHECK_FALSE(cache->page_filter.Any(page + 0x1000, 0x1000));
    CHECK_FALSE(cache->page_filter.Any(page - 0x1000, 0x1000));

    // A write elsewhere keeps the rejection.
    const std::array unrelated{std::pair<DAddr, size_t>{rejected + 0x10000, 4}};
    cache->InvalidateRegions(unrelated);
    CHECK(ShaderCacheTestAccess::IsRejected(*cache, rejected));

    // A write over it, admitted by the filter, lets the program be lowered again.
    const std::array over{std::pair<DAddr, size_t>{rejected, 8}};
    REQUIRE(cache->page_filter.Any(over[0].first, over[0].second));
    cache->InvalidateRegions(over);
    CHECK_FALSE(ShaderCacheTestAccess::IsRejected(*cache, rejected));
}

TEST_CASE("Remembering a rejection twice keeps the first error",
          "[shader_invalidation][page_filter]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    constexpr VAddr rejected = 0x200000;
    ShaderCacheTestAccess::Reject(*cache, rejected);
    ShaderCacheTestAccess::RememberRejection(*cache, rejected);
    CHECK(ShaderCacheTestAccess::IsRejected(*cache, rejected));
    try {
        ShaderCacheTestAccess::Rethrow(*cache, rejected);
        FAIL("Rejection was not rethrown");
    } catch (const std::runtime_error& error) {
        CHECK(std::string_view{error.what()} == "rejected");
    }
}

TEST_CASE("Shader invalidation CPU benchmark", "[.][cache_bench]") {
    Core::DeviceMemory memory;
    Tegra::MaxwellDeviceMemoryManager device_memory{memory};
    auto batched = ShaderCacheTestAccess::Create(device_memory);
    auto sequential = ShaderCacheTestAccess::Create(device_memory);
    std::vector<std::pair<DAddr, size_t>> writes;
    constexpr VAddr base = 0x100000;
    for (unsigned slot = 0; slot < 768; ++slot) {
        writes.emplace_back(base + slot * 65536, 1);
    }
    double sequential_ms{}, batched_ms{};
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
        for (unsigned slot = 0; slot < 1024; ++slot) {
            const auto address = base + slot * 65536;
            if (!ShaderCacheTestAccess::Find(*batched, address)) {
                ShaderCacheTestAccess::Register(*batched, address, 64, slot);
                ShaderCacheTestAccess::Register(*sequential, address, 64, slot);
            }
        }
        auto start = std::chrono::steady_clock::now();
        for (const auto& [address, size] : writes) {
            sequential->InvalidateRegion(address, size);
        }
        sequential_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        start = std::chrono::steady_clock::now();
        batched->InvalidateRegions(writes);
        batched_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        REQUIRE_FALSE(ShaderCacheTestAccess::Find(*batched, base));
        REQUIRE(ShaderCacheTestAccess::Find(*batched, base + 1023 * 65536));
    }
    std::cout << "Shader invalidation: sequential=" << sequential_ms << " ms batched=" << batched_ms
              << " ms (CPU only, not gameplay FPS)\n";
}

namespace {
class TensorPolicyRasterizer final : public VideoCore::RasterizerInterface {
public:
    bool emulate_tensor = false;
    bool EmulatesNativeTensorPrograms() const override { return emulate_tensor; }
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
} // namespace

TEST_CASE("Cached tensor rejections retain the stand-in reason on every dispatch",
          "[shader_invalidation][tensor_standin]") {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    TensorPolicyRasterizer rasterizer;
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    memory.BindRasterizer(&rasterizer);
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    memory.Map(gpu_address, device_address, 0x1000, Tegra::PTEKind::PITCH, false);
    REQUIRE(memory.GpuToCpuAddress(gpu_address) == device_address);
    Tegra::Engines::KeplerCompute compute{memory};
    compute.BindRasterizer(&rasterizer);
    Tegra::MacroEngine macros{true};
    compute.SetNativeMacroMode(&macros, {});
    compute.regs.code_loc.address_low = static_cast<u32>(gpu_address);
    compute.regs.code_loc.address_high = 0;
    compute.launch_description.program_start = 0;
    auto cache = ShaderCacheTestAccess::Create(device_memory);
    ShaderCacheTestAccess::ConfigureCompute(*cache, compute, memory);
    ShaderCacheTestAccess::RejectMatrix(*cache, device_address);

    for (unsigned frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        bool retained_reason = false;
        try {
            ShaderCacheTestAccess::Compute(*cache);
        } catch (const Shader::Exception& error) {
            retained_reason = true;
            CHECK(std::string_view{error.what()} ==
                  "native matrix multiply (not emulated on this host)");
        }
        REQUIRE(retained_reason);
        REQUIRE(ShaderCacheTestAccess::IsRejected(*cache, device_address));
    }
    // Hosts that execute tensor shaders retain the existing cached-failure fast path.
    rasterizer.emulate_tensor = true;
    for (unsigned frame = 0; frame < 3; ++frame) {
        REQUIRE(ShaderCacheTestAccess::Compute(*cache) == nullptr);
        REQUIRE(ShaderCacheTestAccess::IsRejected(*cache, device_address));
    }
}
