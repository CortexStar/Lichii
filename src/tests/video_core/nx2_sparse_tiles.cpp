// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <future>
#include <latch>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/bounded_threadsafe_queue.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nx2_sparse_region.h"
#include "video_core/nx2.h"

using Service::Nx2ParseSparseMapRequests;
using Service::Nx2SparseMapRequest;
using Service::Nx2SparseRegion;
using Service::Nvidia::Devices::Nx2GpuMemoryRegistry;
using Service::Nvidia::Devices::Nx2GpuCommandScope;
using Service::Nvidia::Devices::Nx2GpuMappingPreparation;

namespace {
struct TileEvents {
    TileEvents() {
        auto& registry = Nx2GpuMemoryRegistry::Get();
        registry.Reset();
        registry.AddListener(
            this, [this](u64 gpu, u64 cpu, u64 size) { events.push_back({'m', gpu, cpu, size}); },
            [this](u64 gpu, u64 size) { events.push_back({'u', gpu, 0, size}); },
            [this](u64 cpu, u64 size) { events.push_back({'r', 0, cpu, size}); },
            [this](u64 gpu, u64 size, bool create) {
                events.push_back({static_cast<u64>(create ? 's' : 'd'), gpu, 0, size});
            },
            [this](u64 gpu, u64 cpu, u64 size) { events.push_back({'b', gpu, cpu, size}); },
            [this](u64 gpu, u64 size) { events.push_back({'v', gpu, 0, size}); });
    }
    ~TileEvents() {
        Nx2GpuMemoryRegistry::Get().RemoveListener(this);
        Nx2GpuMemoryRegistry::Get().Reset();
    }
    std::vector<std::array<u64, 4>> events;
};
}

TEST_CASE("Nx2 216 validates complete framing before reading jobs", "[nx2_tiles]") {
    const Nx2SparseMapRequest request{.gpu_address = 0x100000, .size = 0x1000,
                                      .kind = 0x42, .memory_handle = 7, .offset = 0x2000};
    const auto data = std::span{reinterpret_cast<const u8*>(&request), sizeof(request)};
    const auto decoded = Nx2ParseSparseMapRequests(data, 40, 1);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 1);
    CHECK(decoded->at(0).kind == 0x42);
    CHECK(decoded->at(0).memory_handle == 7);
    CHECK(decoded->at(0).offset == 0x2000);
    CHECK_FALSE(Nx2ParseSparseMapRequests(data.first(39), 40, 1));
    CHECK_FALSE(Nx2ParseSparseMapRequests(data, 0, 1));
    CHECK_FALSE(Nx2ParseSparseMapRequests(data, 32, 1));
    CHECK_FALSE(Nx2ParseSparseMapRequests(data, 40, 0));
    REQUIRE(Nx2ParseSparseMapRequests({}, 40, 0));
    CHECK(Nx2ParseSparseMapRequests({}, 40, 0)->empty());
}

TEST_CASE("Nx2 failed backing rollback never closes mapped pages and can retry", "[nx2_tiles]") {
    std::vector<int> calls;
    bool unmapped = false;
    auto unmap = [&] { calls.push_back(1); return unmapped; };
    auto close = [&] { calls.push_back(2); };
    CHECK_FALSE(Service::Nx2TryReleaseBacking(unmap, close));
    CHECK(calls == std::vector<int>{1});
    unmapped = true;
    CHECK(Service::Nx2TryReleaseBacking(unmap, close));
    CHECK(calls == std::vector<int>{1, 1, 2});
}

TEST_CASE("Nx2 216 rejects overflowing and unaligned guest ranges", "[nx2_tiles]") {
    Nx2SparseMapRequest request{.gpu_address = 0x100000, .size = 0x1000,
                                .memory_handle = 1, .offset = 0x2000};
    CHECK(request.Valid());
    CHECK(request.Backing(0x200000, 0x3000) == 0x202000);
    CHECK_FALSE(request.Backing(0x200000, 0x2fff));
    request.offset = std::numeric_limits<u64>::max() - 0xfff;
    CHECK_FALSE(request.Backing(0x200000, 0x3000));
    request.offset = 0;
    CHECK_FALSE(request.Backing(std::numeric_limits<u64>::max() - 0xfff, 0x2000));
    request.gpu_address = std::numeric_limits<u64>::max() - 0xfff;
    CHECK_FALSE(request.Valid());
    request.gpu_address = 0x100001;
    CHECK_FALSE(request.Valid());
    request.gpu_address = 0x100000;
    request.size = 0;
    CHECK_FALSE(request.Valid());
    CHECK_FALSE(request.Backing(0x200000, 0x3000));
}

TEST_CASE("Nx2 guest tiles cancel pending lazy publication without rearming it", "[nx2_tiles]") {
    Nx2SparseRegion state{Nx2SparseRegion::BlockSize + 0x1000, 17};
    const auto pending = state.BeginBacking(0);
    REQUIRE(pending);
    state.StartTiles();
    CHECK_FALSE(state.IsPending(*pending));
    CHECK_FALSE(state.FinishBacking(*pending, true));
    CHECK_FALSE(state.BeginBacking(0));
    CHECK_FALSE(state.MarkWritten(0, 0));
    CHECK_FALSE(state.MarkWritten(state.size - 1, 2));
    CHECK_FALSE(state.MarkWritten(std::numeric_limits<u64>::max(), 1));
    CHECK(state.MarkWritten(Nx2SparseRegion::BlockSize - 0x1000, 0x2000));
    CHECK(state.blocks[0] == Nx2SparseRegion::Block::Written);
    CHECK(state.blocks[1] == Nx2SparseRegion::Block::Written);
    Nx2SparseRegion replacement{state.size, 18};
    REQUIRE(replacement.BeginBacking(0));
    CHECK_FALSE(replacement.FinishBacking(*pending, true));
}

TEST_CASE("Nx2 216 detaches lazy backing before mapping and retiring it", "[nx2_tiles]") {
    TileEvents capture;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    REQUIRE(registry.CreateSparse(0x400000, 0x800000));
    REQUIRE(registry.BackSparse(0x400000, 0x1000000, 0x400000));
    REQUIRE(registry.MapAt(0x2000000, 0x1000000, 0x1000));
    capture.events.clear();
    REQUIRE(registry.MapSparseTile(0x401000, 0x3000000, 0x2000));
    CHECK(capture.events == std::vector<std::array<u64, 4>>{
        {'b', 0x400000, 0, 0x800000}, {'m', 0x401000, 0x3000000, 0x2000}});
    bool retired = false;
    registry.ReleaseBacking(0x1000000, 0x400000, [&] { retired = true; });
    CHECK_FALSE(retired);
    CHECK_FALSE(registry.BackSparse(0x400000, 0x1100000, 0x400000));
    capture.events.clear();
    registry.Unmap(0x2000000, 0x1000);
    CHECK(retired);
    CHECK(capture.events == std::vector<std::array<u64, 4>>{
        {'u', 0x2000000, 0, 0x1000}, {'r', 0, 0x1000000, 0x400000}});
    capture.events.clear();
    REQUIRE(registry.MapSparseTile(0x401000, 0, 0x1000));
    CHECK(capture.events == std::vector<std::array<u64, 4>>{{'v', 0x401000, 0, 0x1000}});
    capture.events.clear();
    int late_owner;
    registry.AddListener(&late_owner,
        [&](u64 gpu, u64 cpu, u64 size) { capture.events.push_back({'m', gpu, cpu, size}); },
        [](u64, u64) {}, {},
        [&](u64 gpu, u64 size, bool) { capture.events.push_back({'s', gpu, 0, size}); },
        [&](u64 gpu, u64 cpu, u64 size) { capture.events.push_back({'b', gpu, cpu, size}); });
    CHECK(capture.events == std::vector<std::array<u64, 4>>{
        {'s', 0x400000, 0, 0x800000}, {'m', 0x402000, 0x3001000, 0x1000}});
    registry.RemoveListener(&late_owner);
}

TEST_CASE("Nx2 216 rejected jobs retain overlays, reservations and native leases", "[nx2_tiles]") {
    TileEvents capture;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    REQUIRE(registry.CreateSparse(0x400000, 0x400000));
    REQUIRE(registry.BackSparse(0x400000, 0x1000000, 0x400000));
    const auto lease = registry.AcquireNative(0x900000, 0x2000);
    REQUIRE(lease);
    capture.events.clear();
    CHECK_FALSE(registry.MapSparseTile(0x3ff000, 0x2000000, 0x2000));
    CHECK_FALSE(registry.MapSparseTile(0x7ff000, 0x2000000, 0x2000));
    CHECK_FALSE(registry.MapSparseTile(0x400000, 0x2000000, 0));
    CHECK_FALSE(registry.MapSparseTile(0x400001, 0x2000000, 0x1000));
    CHECK_FALSE(registry.MapSparseTile(0x900000, 0x2000000, 0x1000));
    CHECK(capture.events.empty());
    CHECK(registry.BackSparse(0x400000, 0x1000000, 0x400000));
    capture.events.clear();
    REQUIRE(registry.MapSparseTile(0x400000, 0x2000000, 0x1000));
    const auto accepted = capture.events;
    CHECK_FALSE(registry.MapSparseTile(0x900000, 0x2100000, 0x1000));
    CHECK(capture.events == accepted);
    CHECK_FALSE(registry.AcquireNative(0x400000, 0x1000));
    capture.events.clear();
    REQUIRE(registry.MapSparseTile(0x900000, 0, 0x2000));
    CHECK(capture.events.empty());
    registry.ReleaseNative(*lease);
    CHECK(capture.events.front()[0] == 'u');
}

TEST_CASE("Nx2 216 lifetime reuse clears tile mode and old late-listener mappings", "[nx2_tiles]") {
    TileEvents capture;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    REQUIRE(registry.CreateSparse(0x400000, 0x400000));
    REQUIRE(registry.MapSparseTile(0x400000, 0x2000000, 0x1000));
    capture.events.clear();
    REQUIRE(registry.DestroySparse(0x400000, 0x400000));
    REQUIRE(registry.CreateSparse(0x400000, 0x1000));
    REQUIRE(registry.BackSparse(0x400000, 0x3000000, 0x1000));
    CHECK(capture.events == std::vector<std::array<u64, 4>>{
        {'d', 0x400000, 0, 0x400000}, {'u', 0x400000, 0, 0x1000},
        {'s', 0x400000, 0, 0x1000}, {'b', 0x400000, 0x3000000, 0x1000}});
}

TEST_CASE("Nx2 publication leaves state locks before bounded synchronous queue waits", "[nx2_tiles]") {
    struct BoundedGPU {
        explicit BoundedGPU(bool synchronous_) : synchronous{synchronous_} {}

        void RunInOrder(std::function<void()>&& work) {
            auto complete = std::make_shared<std::promise<void>>();
            auto future = complete->get_future();
            attempts.fetch_add(1);
            attempts.notify_all();
            queue.EmplaceWait([work = std::move(work), complete] {
                work();
                complete->set_value();
            });
            if (synchronous) {
                future.wait();
            }
        }

        Common::SPSCQueue<std::function<void()>, 1> queue;
        std::atomic<unsigned> attempts{};
        bool synchronous;
    };

    for (const bool synchronous : {false, true}) {
        TileEvents capture;
        BoundedGPU gpu{synchronous};
        auto& registry = Nx2GpuMemoryRegistry::Get();
        REQUIRE(registry.CreateSparse(0x400000, 0x400000));
        registry.RemoveListener(&capture);
        std::vector<int> published;
        registry.AddListener(&gpu, [&](u64 address, u64, u64) {
            Nx2GpuCommandScope::Run(gpu, [&, address] {
                published.push_back(address == 0x1000000 ? 1 : 2);
            });
        }, [](u64, u64) {});
        std::mutex region_mutex;
        bool lazy_backed = false;
        bool first_mapped = false;
        bool second_mapped = false;
        std::atomic<bool> second_mutated{};
        gpu.queue.EmplaceWait([&] {
            std::scoped_lock lock{region_mutex};
            lazy_backed = registry.BackSparse(0x400000, 0x3000000, 0x400000);
            published.push_back(0);
        });
        std::jthread first([&] {
            Nx2GpuCommandScope::ExecuteOnGpu(gpu, [&] {
                std::scoped_lock lock{region_mutex};
                first_mapped = registry.MapSparseTile(0x1000000, 0x2000000, 0x1000);
                Nx2GpuCommandScope::Run(gpu, [&] { published.push_back(11); });
            });
        });
        while (gpu.attempts.load() == 0) {
            gpu.attempts.wait(0);
        }
        std::latch second_started{1};
        std::jthread second([&] {
            second_started.count_down();
            Nx2GpuCommandScope::ExecuteOnGpu(gpu, [&] {
                std::scoped_lock lock{region_mutex};
                second_mapped = registry.MapSparseTile(0x1100000, 0x2100000, 0x1000);
                second_mutated.store(true);
                Nx2GpuCommandScope::Run(gpu, [&] { published.push_back(12); });
            });
        });
        second_started.wait();
        CHECK_FALSE(second_mutated.load());
        std::jthread worker([&] {
            for (int count = 0; count < 3; ++count) {
                gpu.queue.PopWait()();
            }
        });
        first.join();
        second.join();
        worker.join();
        CHECK(first_mapped);
        CHECK(second_mapped);
        CHECK(lazy_backed);
        CHECK(published == std::vector<int>{0, 1, 11, 2, 12});
        registry.RemoveListener(&gpu);
    }
}

TEST_CASE("Nx2 tile ownership waits for an older lazy write and read", "[nx2_tiles]") {
    struct GPU {
        void RunInOrder(std::function<void()>&& work) {
            attempts.fetch_add(1);
            attempts.notify_all();
            queue.EmplaceWait(std::move(work));
        }
        Common::SPSCQueue<std::function<void()>, 1> queue;
        std::atomic<unsigned> attempts{};
    } gpu;
    TileEvents capture;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    REQUIRE(registry.CreateSparse(0x400000, 0x400000));
    registry.RemoveListener(&capture);
    Nx2SparseRegion state{0x400000, 17};
    std::mutex region_mutex;
    u64 visible{};
    u64 old_read{};
    u64 tile_read{};
    bool mapped{};
    bool locks_released{};
    registry.AddListener(&gpu, [&](u64, u64 cpu, u64) {
        Nx2GpuCommandScope::Run(gpu, [&, cpu] {
            locks_released = region_mutex.try_lock();
            if (locks_released) {
                region_mutex.unlock();
            }
            registry.BackSparse(0x400000, 0x3000000, 0x400000);
            visible = cpu;
        });
    }, [](u64, u64) {}, {}, {}, [&](u64, u64 cpu, u64) {
        if (cpu != 0) {
            visible = cpu;
        } else {
            Nx2GpuCommandScope::Run(gpu, [&] { visible = 0; });
        }
    });
    gpu.queue.EmplaceWait([&] {
        std::scoped_lock lock{region_mutex};
        if (auto pending = state.BeginBacking(0)) {
            const bool backed = registry.BackSparse(0x400000, 0x1000000, 0x400000);
            state.FinishBacking(*pending, backed);
        }
        old_read = visible;
    });
    std::jthread service([&] {
        Nx2GpuCommandScope::ExecuteOnGpu(gpu, [&] {
            std::scoped_lock lock{region_mutex};
            mapped = registry.MapSparseTile(0x400000, 0x2000000, 0x1000);
            state.StartTiles();
            Nx2GpuCommandScope::Run(gpu, [&] { tile_read = visible; });
        });
    });
    while (gpu.attempts.load() == 0) {
        gpu.attempts.wait(0);
    }
    std::jthread worker([&] {
        gpu.queue.PopWait()();
        gpu.queue.PopWait()();
    });
    service.join();
    worker.join();
    CHECK(mapped);
    CHECK(old_read == 0x1000000);
    CHECK(tile_read == 0x2000000);
    CHECK(locks_released);
    registry.RemoveListener(&gpu);
}

TEST_CASE("Nx2 GPU ownership boundaries return failures and clean up borrowed scopes", "[nx2_tiles]") {
    struct GPU {
        void RunInOrder(std::function<void()>&& work) {
            if (fail) {
                throw std::runtime_error{"submission failed"};
            }
            if (drop) {
                return;
            }
            ++submissions;
            work();
        }
        bool fail{};
        bool drop{};
        unsigned submissions{};
    } gpu;
    std::vector<int> published;
    std::mutex state_mutex;
    CHECK_FALSE(Nx2GpuCommandScope::ExecuteOnGpu(gpu, [&] {
        std::scoped_lock lock{state_mutex};
        Nx2GpuCommandScope::Execute([&] {
            Nx2GpuCommandScope::Run(gpu, [&] {
                std::scoped_lock callback_lock{state_mutex};
                published.push_back(1);
            });
        });
        return false;
    }));
    CHECK(gpu.submissions == 1);
    CHECK(published == std::vector<int>{1});
    CHECK_THROWS_AS(Nx2GpuCommandScope::ExecuteOnGpu(gpu, [&] {
        std::scoped_lock lock{state_mutex};
        Nx2GpuCommandScope::Run(gpu, [&] {
            std::scoped_lock callback_lock{state_mutex};
            published.push_back(2);
        });
        throw std::runtime_error{"mutation failed"};
    }), std::runtime_error);
    CHECK(published == std::vector<int>{1, 2});
    CHECK_THROWS_AS(Nx2GpuCommandScope::ExecuteOnGpu(gpu, [&] {
        Nx2GpuCommandScope::Run(gpu, [] { throw std::runtime_error{"publication failed"}; });
    }), std::runtime_error);
    gpu.fail = true;
    CHECK_THROWS_AS(Nx2GpuCommandScope::ExecuteOnGpu(gpu, [] { return 7; }), std::runtime_error);
    gpu.fail = false;
    CHECK(Nx2GpuCommandScope::ExecuteOnGpu(gpu, [] { return 7; }) == 7);
    gpu.drop = true;
    CHECK_THROWS_AS(Nx2GpuCommandScope::ExecuteOnGpu(gpu, [] { return 7; }), std::future_error);
    gpu.drop = false;
    Nx2GpuCommandScope::Execute([&] {
        CHECK_THROWS_AS(Nx2GpuCommandScope::ExecuteOnGpu(gpu, [] {}), std::logic_error);
    });
    Nx2GpuCommandScope::Run(gpu, [&] { published.push_back(3); });
    CHECK(published == std::vector<int>{1, 2, 3});
}

TEST_CASE("Nx2 command scopes publish accepted work when nested mutation throws", "[nx2_tiles]") {
    struct GPU {
        void RunInOrder(std::function<void()>&& work) {
            if (fail) {
                throw std::runtime_error{"submission failed"};
            }
            work();
        }
        bool fail{};
    } gpu;
    std::vector<int> published;
    CHECK_THROWS_AS(Nx2GpuCommandScope::Execute([&] {
        Nx2GpuCommandScope::Run(gpu, [&] { published.push_back(1); });
        Nx2GpuCommandScope::Execute([&] {
            Nx2GpuCommandScope::Run(gpu, [&] { published.push_back(2); });
            throw std::runtime_error{"mutation failed"};
        });
    }), std::runtime_error);
    CHECK(published == std::vector<int>{1, 2});
    gpu.fail = true;
    CHECK_THROWS_AS(Nx2GpuCommandScope::Execute([&] {
        Nx2GpuCommandScope::Run(gpu, [&] { published.push_back(3); });
        throw std::runtime_error{"mutation failed"};
    }), std::runtime_error);
    gpu.fail = false;
    Nx2GpuCommandScope::Execute([&] {
        Nx2GpuCommandScope::Run(gpu, [&] { published.push_back(4); });
    });
    CHECK(published == std::vector<int>{1, 2, 4});
}

TEST_CASE("Nx2 queued tile notifications cannot cross ordered region recreation", "[nx2_tiles]") {
    struct GPU {
        void RunInOrder(std::function<void()>&& work) {
            queued.push_back(std::move(work));
        }
        std::vector<std::function<void()>> queued;
    } gpu;
    Tegra::Nx2ClearSparseRegions();
    Nx2GpuCommandScope::Execute([&] {
        Nx2GpuCommandScope::Run(gpu, [] {
            Tegra::Nx2SparseRegionCreated(0x400000, 0x40000, Nx2SparseRegion::BlockSize);
        });
        Nx2GpuCommandScope::Run(gpu, [] {
            Tegra::Nx2SparseTilesMapped(0x400000, 0, 0x10000, true);
        });
    });
    Nx2GpuCommandScope::Execute([&] {
        Nx2GpuCommandScope::Run(gpu, [] { Tegra::Nx2SparseRegionDestroyed(0x400000); });
        Nx2GpuCommandScope::Run(gpu, [] {
            Tegra::Nx2SparseRegionCreated(0x400000, 0x30000, Nx2SparseRegion::BlockSize);
            Tegra::Nx2SparseTilesMapped(0x400000, 0x20000, 0x10000, true);
        });
    });
    REQUIRE(gpu.queued.size() == 2);
    CHECK(Tegra::Nx2SparseDataRanges(0x400000, 0x40000).empty());
    gpu.queued[0]();
    CHECK(Tegra::Nx2SparseDataRanges(0x400000, 0x40000) ==
          std::vector<std::pair<u64, u64>>{{0, 0x10000}});
    gpu.queued[1]();
    CHECK(Tegra::Nx2SparseDataRanges(0x400000, 0x30000) ==
          std::vector<std::pair<u64, u64>>{{0x20000, 0x10000}});
    Tegra::Nx2ClearSparseRegions();
}

TEST_CASE("Nx2 accepted tile overlays retain the other page after a partial unmap", "[nx2_tiles]") {
    struct GPU {
        void RunInOrder(std::function<void()>&& work) {
            work();
        }
    } gpu;
    TileEvents capture;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    REQUIRE(registry.CreateSparse(0x400000, 0x10000));
    Tegra::Nx2SparseRegionCreated(0x400000, 0x10000, Nx2SparseRegion::BlockSize);
    const auto apply = [&](u64 address, u64 cpu) {
        return Nx2GpuCommandScope::Execute([&] {
            const bool accepted = registry.MapSparseTile(address, cpu, 0x1000);
            if (accepted) {
                Nx2GpuCommandScope::Run(gpu, [=] {
                    Tegra::Nx2SparseTilesMapped(0x400000, address - 0x400000, 0x1000, cpu != 0);
                });
            }
            return accepted;
        });
    };
    REQUIRE(apply(0x400000, 0x1000000));
    REQUIRE(apply(0x402000, 0x2000000));
    REQUIRE(apply(0x400000, 0));
    CHECK_FALSE(apply(0x40f800, 0x3000000));
    CHECK(Tegra::Nx2SparseDataRanges(0x400000, 0x10000) ==
          std::vector<std::pair<u64, u64>>{{0x2000, 0x1000}});
    capture.events.clear();
    REQUIRE(apply(0x402000, 0));
    CHECK(capture.events == std::vector<std::array<u64, 4>>{{'v', 0x402000, 0, 0x1000}});
    CHECK(Tegra::Nx2SparseDataRanges(0x400000, 0x10000).empty());
    Tegra::Nx2ClearSparseRegions();
}

TEST_CASE("Nx2 tile preparation rolls back a failed address space without losing reused backing",
          "[nx2_tiles]") {
    TileEvents capture;
    auto& registry = Nx2GpuMemoryRegistry::Get();
    std::array<int, 2> owners{};
    std::array<std::map<u64, u64>, 2> allocated;
    std::vector<unsigned> prepared_order;
    unsigned rolled_back{};
    unsigned mapped{};
    bool fail_second = false;
    for (unsigned asid = 0; asid < owners.size(); ++asid) {
        registry.AddListener(&owners[asid],
            [&, asid](u64, u64 cpu, u64 size) {
                CHECK(allocated[asid].contains(cpu));
                CHECK(allocated[asid].at(cpu) >= size);
                ++mapped;
            }, [](u64, u64) {}, {}, {}, {}, {},
            [&, asid](u64, u64 cpu, u64 size) -> std::optional<Nx2GpuMappingPreparation> {
                prepared_order.push_back(asid);
                if (asid == 1 && fail_second) {
                    return std::nullopt;
                }
                if (cpu == 0) {
                    return Nx2GpuMappingPreparation{};
                }
                if (allocated[asid].contains(cpu)) {
                    return Nx2GpuMappingPreparation{};
                }
                allocated[asid][cpu] = size;
                return Nx2GpuMappingPreparation{[&, asid, cpu] {
                    allocated[asid].erase(cpu);
                    ++rolled_back;
                }};
            });
    }
    REQUIRE(registry.MapSparseTile(0x1000000, 0x2000000, 0x1000));
    CHECK(mapped == 2);
    CHECK(rolled_back == 0);
    fail_second = true;
    prepared_order.clear();
    capture.events.clear();
    CHECK_FALSE(registry.MapSparseTile(0x1000000, 0x3000000, 0x1000));
    CHECK(prepared_order == std::vector<unsigned>{0, 1});
    CHECK(mapped == 2);
    CHECK(rolled_back == 1);
    CHECK(capture.events.empty());
    CHECK(allocated[0] == std::map<u64, u64>{{0x2000000, 0x1000}});
    CHECK(allocated[1] == allocated[0]);
    CHECK_FALSE(registry.MapSparseTile(0x1100000, 0x2000000, 0x1000));
    CHECK(mapped == 2);
    CHECK(rolled_back == 1);
    CHECK(allocated[1] == allocated[0]);
    CHECK(capture.events.empty());
    std::vector<std::array<u64, 3>> replay;
    int observer;
    registry.AddListener(&observer, [&](u64 gpu, u64 cpu, u64 size) {
        replay.push_back({gpu, cpu, size});
    }, [](u64, u64) {});
    CHECK(replay == std::vector<std::array<u64, 3>>{{0x1000000, 0x2000000, 0x1000}});
    registry.RemoveListener(&observer);
    REQUIRE(registry.CreateSparse(0x400000, 0x400000));
    CHECK_FALSE(registry.MapSparseTile(0x400000, 0x3000000, 0x1000));
    CHECK(registry.BackSparse(0x400000, 0x5000000, 0x400000));
    fail_second = false;
    REQUIRE(registry.MapSparseTile(0x1000000, 0x3000000, 0x1000));
    CHECK(mapped == 4);
    CHECK(allocated[0].contains(0x3000000));
    CHECK(allocated[1].contains(0x3000000));
    allocated[1].erase(0x3000000);
    REQUIRE(registry.MapSparseTile(0x1000000, 0x3000000, 0x1000));
    CHECK(mapped == 6);
    CHECK(allocated[1].contains(0x3000000));
    for (auto& owner : owners) {
        registry.RemoveListener(&owner);
    }
}
