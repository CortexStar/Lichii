// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/front_qmd.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace VideoCommon {

struct GpuSplitTestAccess {
    static void Initialize(GpuSplit& split, Core::System& system, u64 position = 0) {
        split.system = &system;
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.write_position = position;
        split.consumed_cache = position;
        split.published_local = position;
        split.publish_every = GpuSplit::RingWords;
    }

    static u64 End(const GpuSplit& split) {
        return split.write_position;
    }

    static void Start(GpuSplit& split, Core::System& system) {
        Initialize(split, system);
        split.active = true;
        split.back_thread = std::jthread([&split](std::stop_token stop) { split.BackLoop(stop); });
    }

    static u64 NearWrap() {
        return GpuSplit::RingWords - 1;
    }

    /// The narrow drains' write index without a back thread: writes are noted at `position`.
    static void InitializeNarrow(GpuSplit& split) {
        split.front_macros = true;
        split.page_slots = std::make_unique<GpuSplit::PageSlot[]>(GpuSplit::PageBuckets * 4);
        split.word_slots = std::make_unique<GpuSplit::WordSlot[]>(GpuSplit::WordBuckets * 4);
        split.granule_slots = std::make_unique<GpuSplit::WordSlot[]>(GpuSplit::GranuleBuckets * 4);
    }
    static void SetPositions(GpuSplit& split, u64 written, u64 consumed) {
        split.write_position = written;
        split.consumed_cache = consumed;
    }
    /// A back that waits for work: Commit publishes each entry at once.
    static void SetBackIdle(GpuSplit& split, bool idle) {
        split.back_idle.store(idle, std::memory_order_relaxed);
    }
    static u64 Published(const GpuSplit& split) {
        return split.published.load(std::memory_order_relaxed);
    }
    /// SetPositions, with the back's own position too (what Ran reads).
    static void SetBack(GpuSplit& split, u64 written, u64 consumed) {
        SetPositions(split, written, consumed);
        split.consumed.store(consumed, std::memory_order_relaxed);
    }
    static u64 Target(const GpuSplit& split, const void* space, GPUVAddr address, u64 size) {
        return split.PendingWriteTarget(space, address, size, 0);
    }

    static u64 Execute(GpuSplit& split, u64 position, u64 available) {
        const size_t index = position & GpuSplit::RingMask;
        const size_t size = split.Execute(
            &split.ring[index], std::min<u64>(available - position, GpuSplit::RingWords - index));
        return position + (size != 0 ? size : GpuSplit::RingWords - index);
    }
};

} // namespace VideoCommon

namespace {

using Tegra::Engines::Maxwell3D;

constexpr u32 CbSize = MAXWELL3D_REG_INDEX(const_buffer.size);
constexpr u32 CbAddressHigh = MAXWELL3D_REG_INDEX(const_buffer.address_high);
constexpr u32 CbAddressLow = MAXWELL3D_REG_INDEX(const_buffer.address_low);
constexpr u32 CbOffset = MAXWELL3D_REG_INDEX(const_buffer.offset);
constexpr u32 CbData = MAXWELL3D_REG_INDEX(const_buffer.buffer);
constexpr u32 DrawBegin = MAXWELL3D_REG_INDEX(draw.begin);
constexpr u32 DrawEnd = MAXWELL3D_REG_INDEX(draw.end);
constexpr u32 ShadowControl = MAXWELL3D_REG_INDEX(shadow_ram_control);

// Executable methods without memory or macro effects a null rasterizer cannot take.
constexpr std::array<u32, 16> executable_methods{
    DrawBegin,
    DrawEnd,
    MAXWELL3D_REG_INDEX(vertex_buffer.first),
    MAXWELL3D_REG_INDEX(vertex_buffer.count),
    MAXWELL3D_REG_INDEX(index_buffer.first),
    MAXWELL3D_REG_INDEX(index_buffer.count),
    MAXWELL3D_REG_INDEX(topology_override),
    MAXWELL3D_REG_INDEX(bind_groups[0].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[1].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[2].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[3].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[4].raw_config),
    MAXWELL3D_REG_INDEX(vertex_array_instance_first),
    MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent),
    Maxwell3D::RootTables::SelectMethod,
    Maxwell3D::RootTables::LoadMethod,
};

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Maxwell3D slow{memory};
    Maxwell3D fast{memory};
    std::vector<u32> state_methods;

    Fixture() {
        slow.fast_maxwell3d = false;
        fast.fast_maxwell3d = true;
        for (auto* engine : {&slow, &fast}) {
            engine->BindRasterizer(&rasterizer);
            VideoCommon::Dirty::SetupDirtyFlags(engine->dirty.tables);
            // Give every register a flag so that a missed or extra write shows.
            for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
                if (engine->dirty.tables[1][method] == 0) {
                    engine->dirty.tables[1][method] =
                        static_cast<u8>(VideoCommon::Dirty::LastCommonEntry + method % 61);
                }
            }
            engine->dirty.flags.reset();
            // An unmapped buffer: the upload writes nothing, the offset still advances.
            engine->CallMethod(system, CbAddressHigh, 0x1, true);
            engine->CallMethod(system, CbAddressLow, 0x2000'0000, true);
            engine->CallMethod(system, CbSize, 0x10000, true);
        }
        for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
            if (!slow.execution_mask[method] && (method < CbSize || method > CbAddressLow)) {
                state_methods.push_back(method);
            }
        }
    }

    void Call(u32 method, u32 argument, bool is_last) {
        slow.CallMethod(system, method, argument, is_last);
        fast.CallMethod(system, method, argument, is_last);
    }

    void CallMulti(u32 method, const std::vector<u32>& words) {
        const u32 amount = static_cast<u32>(words.size());
        slow.CallMultiMethod(system, method, words.data(), amount, amount);
        fast.CallMultiMethod(system, method, words.data(), amount, amount);
    }

    void Compare() const {
        REQUIRE(fast.regs.reg_array == slow.regs.reg_array);
        REQUIRE(fast.shadow_state.reg_array == slow.shadow_state.reg_array);
        REQUIRE(fast.shadow_state.shadow_ram_control == slow.shadow_state.shadow_ram_control);
        REQUIRE(fast.dirty.flags == slow.dirty.flags);
        const auto& a = fast.draw_manager.draw_state;
        const auto& b = slow.draw_manager.draw_state;
        REQUIRE(a.topology == b.topology);
        REQUIRE(a.draw_mode == b.draw_mode);
        REQUIRE(a.draw_indexed == b.draw_indexed);
        REQUIRE(a.draw_element_count == b.draw_element_count);
        REQUIRE(a.base_index == b.base_index);
        REQUIRE(a.base_instance == b.base_instance);
        REQUIRE(a.instance_count == b.instance_count);
        REQUIRE(a.drawn_instances == b.drawn_instances);
        REQUIRE(std::memcmp(&a.vertex_buffer, &b.vertex_buffer, sizeof(a.vertex_buffer)) == 0);
        REQUIRE(std::memcmp(&a.index_buffer, &b.index_buffer, sizeof(a.index_buffer)) == 0);
        for (size_t stage = 0; stage < fast.state.shader_stages.size(); ++stage) {
            for (size_t slot = 0; slot < Maxwell3D::Regs::MaxConstBuffers; ++slot) {
                const auto& x = fast.state.shader_stages[stage].const_buffers[slot];
                const auto& y = slow.state.shader_stages[stage].const_buffers[slot];
                REQUIRE(x.address == y.address);
                REQUIRE(x.size == y.size);
                REQUIRE(x.enabled == y.enabled);
            }
        }
        REQUIRE(fast.root_tables.tables == slow.root_tables.tables);
        REQUIRE(fast.root_tables.selected == slow.root_tables.selected);
        REQUIRE(fast.root_tables.offset == slow.root_tables.offset);
    }
};

u32 RandomArgument(std::mt19937& rng, u32 method, const Maxwell3D& engine) {
    // Mostly values the register already holds or small ones, as the macros write them.
    switch (rng() % 4) {
    case 0:
        return engine.regs.reg_array[method];
    case 1:
        return rng() % 4;
    default:
        break;
    }
    if (method == DrawBegin) {
        // Instance id in bits 26-27: first, subsequent (an instanced loop) or unchanged.
        return (static_cast<u32>(rng() % 3) << 26) | (rng() % 8);
    }
    if (method == Maxwell3D::RootTables::SelectMethod) {
        return (rng() % 8) | ((rng() % 64 * 4) << 8);
    }
    return rng() % 64;
}

} // Anonymous namespace

TEST_CASE("Maxwell3D fast method dispatch matches CallMethod on random method streams",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    for (const u32 seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        CAPTURE(seed);
        std::mt19937 rng{seed};
        for (u32 step = 0; step < 20000; ++step) {
            CAPTURE(step);
            const u32 kind = rng() % 100;
            if (kind < 45) {
                const u32 method = f.state_methods[rng() % f.state_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 75) {
                const u32 method = executable_methods[rng() % executable_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 85) {
                f.Call(CbData + rng() % 16, rng(), true);
            } else if (kind < 88) {
                f.Call(CbOffset, (rng() % 0x4000) * 4, true);
            } else if (kind < 92) {
                std::vector<u32> words(1 + rng() % 8);
                for (u32& word : words) {
                    word = rng() % 3 == 0 ? rng() : rng() % 4;
                }
                const u32 method = rng() % 2 == 0
                                       ? CbData + rng() % 16
                                       : f.state_methods[rng() % f.state_methods.size()];
                f.CallMulti(method, words);
            } else if (kind < 94) {
                // Track, track with filter, passthrough or replay.
                f.Call(ShadowControl, rng() % 4, true);
            } else if (kind < 97) {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->dirty.flags.reset();
                }
            } else {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->draw_manager.DrawDeferred(*engine);
                }
            }
            f.Compare();
        }
    }
}

TEST_CASE("Maxwell3D fast method dispatch issues deferred instanced draws as CallMethod does",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    constexpr u32 subsequent = 1u << 26;
    const auto draw = [&f](u32 begin) {
        f.Call(DrawBegin, begin, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.first), 0, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3, true);
        f.Call(DrawEnd, 0, true);
        f.Compare();
    };
    draw(0);
    draw(subsequent);
    draw(subsequent);
    REQUIRE(f.fast.draw_manager.HasDeferredDraw());
    // A constant buffer upload, a bind and state changes each end the loop.
    f.Call(CbData, 7, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(bind_groups[4].raw_config), 1 | (3 << 4), true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(topology_override), 5, true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(global_base_instance_index), 9, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
}

TEST_CASE("Maxwell3D constant buffer word runs match single CallMethod writes",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    std::mt19937 rng{11};
    constexpr u32 subsequent = 1u << 26;
    for (u32 step = 0; step < 20000; ++step) {
        CAPTURE(step);
        switch (rng() % 8) {
        case 0:
            f.Call(ShadowControl, rng() % 4, true);
            break;
        case 1:
            // Near the end too, where words are dropped.
            f.Call(CbSize, rng() % 2 == 0 ? 0x10000 : 0x100 + rng() % 0x40, true);
            break;
        case 2:
            f.Call(CbOffset, rng() % 3 == 0 ? 0xF0 + (rng() % 8) * 4 : (rng() % 0x400) * 4, true);
            break;
        case 3:
            // An instanced loop the first word ends.
            f.Call(DrawBegin, rng() % 2 == 0 ? 0 : subsequent, true);
            f.Call(DrawEnd, 0, true);
            break;
        default: {
            const u32 count = 1 + rng() % 64;
            std::vector<u32> methods(count);
            std::vector<u32> arguments(count);
            const u32 first = CbData + rng() % 16;
            for (u32 i = 0; i < count; ++i) {
                methods[i] = rng() % 2 == 0 ? first : CbData + (first - CbData + i) % 16;
                arguments[i] = rng() % 2 == 0 ? rng() : rng() % 4;
            }
            for (u32 i = 0; i < count; ++i) {
                f.slow.CallMethod(f.system, methods[i], arguments[i], true);
            }
            f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), count);
            break;
        }
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching respects device remaps within GPU big pages",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    auto* replacement = f.physical.GetPointerFromRaw<u8>(0x30000);
    for (u64 page = 0; page < 16; ++page) {
        table.entries.GetAndFault(4 + page)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - 0x4000);
    }
    table.entries.GetAndFault(0x20).Store(false, Common::PageType::Memory, 1,
                                        reinterpret_cast<uintptr_t>(replacement) - 0x20000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x10000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x10000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.device_memory.Unmap(0x11000, 0x1000);
    f.device_memory.Map(0x11000, 0x20000, 0x1000, asid);
    REQUIRE(f.memory.GetPointer(0x100ffc) == backing + 0xffc);
    REQUIRE(f.memory.GetPointer(0x101000) == replacement);

    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x10000);
        std::memset(replacement, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0x100ffc);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 first{}, second{}, stale{};
        std::memcpy(&first, backing + 0xffc, sizeof(first));
        std::memcpy(&second, replacement, sizeof(second));
        std::memcpy(&stale, backing + 0x1000, sizeof(stale));
        CAPTURE(engine == &f.fast);
        CAPTURE(stale);
        REQUIRE(first == 0x12345678);
        REQUIRE(second == 0x87654321);
        REQUIRE(stale == 0);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("GpuSplit dispatch preserves mapped uploads and macro boundaries",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    for (u64 page = 0; page < 3; ++page) {
        const VAddr virtual_address = 0x4000 + page * 0x1000;
        auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000 + page * 0x8000);
        table.entries.GetAndFault(virtual_address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - virtual_address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x3000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(device_address, 0x4000, 0x3000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x3000, Tegra::PTEKind::PITCH, false);

    struct Snapshot {
        std::array<u8, 0x3000> bytes{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> regs{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> shadow{};
        Maxwell3D::DirtyState::Flags dirty{};
        GPUVAddr segment{};
    };
    const auto snapshot = [&](const Maxwell3D& engine) {
        Snapshot result;
        f.memory.ReadBlockUnsafe(gpu_address, result.bytes.data(), result.bytes.size());
        result.regs = engine.regs.reg_array;
        result.shadow = engine.shadow_state.reg_array;
        result.dirty = engine.dirty.flags;
        result.segment = engine.current_dma_segment;
        return result;
    };

    for (const bool wrap : {false, true}) {
        CAPTURE(wrap);
        std::vector<Snapshot> expected;
        for (auto* engine : {&f.slow, &f.fast}) {
            VideoCommon::GpuSplit split;
            struct ProbeEngine final : Tegra::Engines::EngineInterface {
                u32 writes{};
                void CallMethod(Core::System&, u32, u32, bool) override {
                    ++writes;
                }
                void CallMultiMethod(Core::System&, u32, const u32*, u32 amount, u32) override {
                    writes += amount;
                }
            } probe;
            const u64 start = wrap ? Access::NearWrap() : 0;
            Access::Initialize(split, f.system, start);
            std::vector<u64> boundaries;
            std::array<u8, 0x3000> cleared{};
            f.memory.WriteBlockUnsafe(gpu_address, cleared.data(), cleared.size());
            engine->current_dma_segment = 0;
            engine->dirty.flags.reset();
            const auto state = [&](u32 method, u32 value) {
                split.Method(engine, method, value, true, nullptr);
                boundaries.push_back(Access::End(split));
            };
            const auto direct = [&](u32 method, u32 value) {
                split.MacroWrite(engine, method, value);
                boundaries.push_back(Access::End(split));
            };
            size_t observed{};
            const auto inspect = [&] {
                split.Defer([&, engine] {
                    auto actual = snapshot(*engine);
                    if (engine == &f.slow) {
                        expected.push_back(std::move(actual));
                    } else {
                        REQUIRE(observed < expected.size());
                        const auto& reference = expected[observed];
                        REQUIRE(actual.bytes == reference.bytes);
                        REQUIRE(actual.regs == reference.regs);
                        REQUIRE(actual.shadow == reference.shadow);
                        REQUIRE(actual.dirty == reference.dirty);
                        REQUIRE(actual.segment == reference.segment);
                    }
                    ++observed;
                });
                boundaries.push_back(Access::End(split));
            };
            state(ShadowControl, 0);
            state(CbAddressHigh, 0);
            state(CbAddressLow, static_cast<u32>(gpu_address));
            state(CbSize, 0x3000);
            state(CbOffset, 0xffb);
            direct(MAXWELL3D_REG_INDEX(global_base_instance_index), 17);
            inspect();
            for (u32 i = 0; i < 129; ++i) {
                if (i % 2 == 0) {
                    direct(CbData + i % 16, 0x12340000 + i);
                } else {
                    state(CbData + i % 16, 0x56780000 + i);
                }
            }
            inspect();
            state(ShadowControl, 3);
            state(CbOffset, 0x1ffd);
            for (u32 i = 0; i < 33; ++i) {
                direct(CbData + i % 16, 0xdeadbeef);
            }
            inspect();
            state(ShadowControl, 2);
            state(CbOffset, 0x1ffd);
            direct(DrawBegin, 0);
            direct(DrawEnd, 0);
            direct(DrawBegin, 1u << 26);
            direct(DrawEnd, 0);
            direct(CbData, 0x11223344);
            direct(CbData + 1, 0xaabbccdd);
            const GPUVAddr segment = 0x12345678;
            split.Method(engine, CbData + 2, 0x87654321, false, &segment);
            boundaries.push_back(Access::End(split));
            direct(CbData + 3, 0x76543210);
            split.MacroWrite(&probe, 0x10, 42);
            boundaries.push_back(Access::End(split));
            direct(CbData + 4, 0x65432100);
            const std::array<u32, 3> multi{19, 20, 21};
            split.MultiMethod(engine, CbData, multi.data(), 3, 3, &segment);
            boundaries.push_back(Access::End(split));
            inspect();
            state(CbSize, 7);
            state(CbOffset, 0);
            direct(CbData, 0x99aabbcc);
            direct(CbData + 1, 0xddeeff00);
            inspect();

            u64 position = start;
            if (wrap) {
                for (const u64 available : boundaries) {
                    while (position < available) {
                        position = Access::Execute(split, position, available);
                        REQUIRE(position <= available);
                    }
                }
            } else {
                while (position < Access::End(split)) {
                    position = Access::Execute(split, position, Access::End(split));
                    REQUIRE(position <= Access::End(split));
                }
            }
            REQUIRE(observed == expected.size());
            REQUIRE(probe.writes == 1);
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching preserves words at the GPU address space end",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(backing) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x1000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0xfffff000, 0x10000, 0x1000, Tegra::PTEKind::PITCH, false);
    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0xfffffffcu);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 word{};
        std::memcpy(&word, backing + 0xffc, sizeof(word));
        CAPTURE(engine == &f.fast);
        REQUIRE(word == 0x12345678);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("Maxwell3D fast method dispatch cost", "[.][maxwell_fast_method_bench]") {
    Fixture f;
    // Per draw, as the back thread sees Ocarina's: state writes the front did not drop, a
    // constant buffer update, three binds and a draw.
    std::vector<std::pair<u32, u32>> stream;
    std::mt19937 rng{1};
    for (u32 draw = 0; draw < 4096; ++draw) {
        for (u32 i = 0; i < 12; ++i) {
            stream.emplace_back(f.state_methods[rng() % 512], rng() % 8);
        }
        stream.emplace_back(CbOffset, 0x640);
        stream.emplace_back(CbData, draw);
        stream.emplace_back(CbData + 1, 0);
        stream.emplace_back(CbData + 2, draw * 3);
        for (const u32 stage : {0u, 2u, 4u}) {
            stream.emplace_back(MAXWELL3D_REG_INDEX(bind_groups[0].raw_config) + stage * 8,
                                1 | (rng() % 8) << 4);
        }
        stream.emplace_back(DrawBegin, 0);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.first), rng() % 4);
        stream.emplace_back(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3 + rng() % 4);
        stream.emplace_back(DrawEnd, 0);
    }
    const auto run = [&](Maxwell3D& engine) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 round = 0; round < 50; ++round) {
            for (const auto& [method, argument] : stream) {
                engine.CallMethod(f.system, method, argument, true);
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               (50.0 * static_cast<double>(stream.size()));
    };
    run(f.slow);
    run(f.fast);
    const double slow_ns = run(f.slow);
    const double fast_ns = run(f.fast);
    WARN("CallMethod: " << slow_ns << " ns per method, fast path " << fast_ns << " ns");

    // Three constant buffer words (a draw macro's c[0][0x640..0x648]) one by one or as a run.
    constexpr std::array<u32, 3> methods{CbData, CbData + 1, CbData + 2};
    constexpr std::array<u32, 3> arguments{1, 2, 3};
    const auto words = [&](bool as_run) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 1'000'000; ++i) {
            f.fast.CallMethod(f.system, CbOffset, 0x640, true);
            if (as_run) {
                f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), 3);
            } else {
                for (u32 w = 0; w < 3; ++w) {
                    f.fast.CallMethod(f.system, methods[w], arguments[w], true);
                }
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               1'000'000.0;
    };
    words(false);
    words(true);
    const double single_ns = words(false);
    const double run_ns = words(true);
    WARN("Three constant buffer words: " << single_ns << " ns one by one, " << run_ns
                                         << " ns as a run (unmapped buffer)");
}

TEST_CASE("Full split drains preserve aliased uploads, partial coverage and queued remaps",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr VAddr cpu_address = 0x4000;
    constexpr DAddr device_address = 0x10000;
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr GPUVAddr alias = 0x200000;
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    for (u64 page = 0; page < 2; ++page) {
        const VAddr address = cpu_address + page * 0x1000;
        table.entries.GetAndFault(address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing + page * 0x1000) - address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    f.device_memory.Map(device_address, cpu_address, 0x2000, asid);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x2000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x2000, Tegra::PTEKind::PITCH, false);
    f.memory.Map(alias, device_address, 0x1000, Tegra::PTEKind::PITCH, false);
    std::memset(backing, 0, 0x2000);

    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    const auto state = [&](u32 method, u32 value) {
        split.Method(&f.fast, method, value, true, nullptr);
    };
    const auto upload = [&](GPUVAddr address, u32 size, std::vector<u32> words) {
        state(ShadowControl, 0);
        state(CbAddressHigh, static_cast<u32>(address >> 32));
        state(CbAddressLow, static_cast<u32>(address));
        state(CbSize, size);
        state(CbOffset, 0);
        split.MultiMethod(&f.fast, CbData, words.data(), static_cast<u32>(words.size()),
                          static_cast<u32>(words.size()), nullptr);
        std::ranges::fill(words, 0xdeadbeef);
    };

    upload(gpu_address, 16, {1, 2, 3, 4});
    upload(alias + 4, 4, {20, 30});
    upload(gpu_address + 0x1000, 8, {50, 60});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    std::array<u32, 4> read{};
    f.memory.ReadBlockUnsafe(gpu_address, read.data(), sizeof(read));
    REQUIRE(read == std::array<u32, 4>{1, 20, 3, 4});
    REQUIRE(f.memory.Read<u32>(alias + 4) == 20);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 50);

    split.Defer([&] {
        f.memory.Map(alias, device_address + 0x1000, 0x1000, Tegra::PTEKind::PITCH, false);
    });
    upload(alias, 4, {70});
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(f.memory.Read<u32>(gpu_address) == 1);
    REQUIRE(f.memory.Read<u32>(gpu_address + 0x1000) == 70);
    REQUIRE(f.memory.Read<u32>(alias + 4) == 60);
}

TEST_CASE("Split consumption does not complete deferred downstream callbacks",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    std::vector<std::function<void()>> deferred;
    std::array<u32, 3> output{};
    for (u32 i = 0; i < output.size(); ++i) {
        split.Defer([&, i] { deferred.emplace_back([&, i] { output[i] = i + 1; }); });
    }
    split.Drain(VideoCommon::DrainSite::MacroDmaRead);
    REQUIRE(deferred.size() == 3);
    REQUIRE(output == std::array<u32, 3>{});
    split.Run([&] {
        for (auto& operation : deferred) {
            operation();
        }
        deferred.clear();
    });
    REQUIRE(output == std::array<u32, 3>{1, 2, 3});
}

TEST_CASE("Split restart resets writer positions without replaying old releases",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    u32 released{};
    u32 release_count{};
    for (u32 session = 1; session <= 3; ++session) {
        Access::Start(split, f.system);
        SCOPE_EXIT { split.Stop(); };
        REQUIRE(Access::End(split) == 0);
        REQUIRE_FALSE(split.WaitForWriters());
        REQUIRE_FALSE(split.WaitForSync());
        split.Defer([&, session] {
            released = session;
            ++release_count;
        });
        split.NoteWriter();
        split.NoteSync();
        split.Drain(VideoCommon::DrainSite::Acquire);
        REQUIRE(released == session);
        REQUIRE(release_count == session);
        split.Stop();
        REQUIRE_FALSE(split.Active());
        REQUIRE_FALSE(split.BackRunning());
        REQUIRE(Access::End(split) == 0);
    }
}

TEST_CASE("Split fence releases skip the drain only at a position the back found quiet",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    VideoCommon::GpuSplit split;
    struct Check {
        std::atomic<bool> quiet{true};
        std::atomic<u64> epoch{7};
    } check;
    split.SetFenceQuietCheck(
        [](void* argument, u64* epoch) {
            auto& state = *static_cast<Check*>(argument);
            *epoch = state.epoch.load();
            return state.quiet.load();
        },
        &check);
    Access::Start(split, f.system);
    SCOPE_EXIT { split.Stop(); };
    u64 epoch{};
    split.Defer([] {});
    split.Drain(VideoCommon::DrainSite::Acquire);
    REQUIRE(split.FencesQuietNow(epoch));
    REQUIRE(epoch == 7);
    // Work sent since the check: not quiet until the back has run it and checked again.
    std::atomic<bool> release{};
    split.Defer([&] {
        while (!release.load()) {
            std::this_thread::yield();
        }
    });
    split.Publish();
    REQUIRE_FALSE(split.FencesQuietNow(epoch));
    check.quiet = false;
    release = true;
    split.Drain(VideoCommon::DrainSite::Acquire);
    REQUIRE_FALSE(split.FencesQuietNow(epoch));
    check.quiet = true;
    check.epoch = 8;
    split.Defer([] {});
    split.Drain(VideoCommon::DrainSite::Acquire);
    REQUIRE(split.FencesQuietNow(epoch));
    REQUIRE(epoch == 8);
    split.Stop();
    REQUIRE_FALSE(split.FencesQuietNow(epoch));
    split.SetFenceQuietCheck(nullptr, nullptr);
}

TEST_CASE("Maxwell3D takes its macros back from the split's front once the split stops",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    constexpr u32 StartAddressPointer = MAXWELL3D_REG_INDEX(load_mme.start_address_ptr);
    constexpr u32 StartAddress = MAXWELL3D_REG_INDEX(load_mme.start_address);
    const u32 state = f.state_methods.front();
    // The front takes the macros over and tracks a state write in its shadow RAM copy.
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    REQUIRE(engine.shadow_state.reg_array[state] != 0x1234);
    // The split stopped: the next engine method runs on this thread, not the split's back.
    engine.CallMethod(f.system, StartAddressPointer, 0, true);
    engine.CallMethod(f.system, StartAddress, 0x40, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x1234);
    CHECK(engine.GetNativeMacroPositions()[0] == 0x40);
}

TEST_CASE("Maxwell3D keeps state written after the split stops over the front's shadow RAM",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    const u32 state = f.state_methods.front();
    engine.FrontCallMethod(f.system, split, state, 0x1234, true);
    // After the stop, DmaPusher sinks a state write and consumes the sink before the next
    // executable method: the handback must come first so that write wins.
    engine.method_sink.emplace_back(state, 0x5678);
    engine.ConsumeSink(f.system);
    engine.CallMethod(f.system, MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), 0, true);
    CHECK(engine.shadow_state.reg_array[state] == 0x5678);
    CHECK(engine.regs.reg_array[state] == 0x5678);
}

TEST_CASE("Maxwell3D's split front uploads macro code sent as one multi-method run",
          "[video_core][gpu_split_dependencies]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    auto& engine = f.fast;
    engine.SetNativeTuringMacroMode(true);
    // GCC 13's loop unswitching dropped these words from the front's per-word loop (#500).
    constexpr u32 pointer = 0x262;
    constexpr std::array<u32, 6> code{0x740003, 0x18c00000, 0x318c0001,
                                      0x740003, 0x18c00000, 0x318c0000};
    engine.FrontCallMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), pointer,
                           true);
    engine.FrontCallMultiMethod(f.system, split, MAXWELL3D_REG_INDEX(load_mme.instruction),
                                code.data(), code.size(), code.size());
    const auto& macros = engine.GetNativeMacroEngine();
    REQUIRE(macros.native_write_offset == pointer * 3 + code.size());
    CHECK(std::equal(code.begin(), code.end(), macros.native_code.begin() + pointer * 3));
}

TEST_CASE("GpuSplit narrow drains wait only for the recorded writes that may overlap a read",
          "[video_core][gpu_split]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    VideoCommon::GpuSplit split;
    Access::InitializeNarrow(split);
    int space_a = 0;
    int space_b = 0;
    // A page-granular write (an inline upload) at position 10.
    Access::SetPositions(split, 10, 0);
    split.NoteWrite(&space_a, 0x10'0000, 0x40);
    // A release (word-granular) at position 20, in another page.
    Access::SetPositions(split, 20, 0);
    split.NoteWrite(&space_a, 0x20'0010, 4, true);

    // A read in the uploaded page waits for the upload, one in an untouched page for nothing.
    REQUIRE(Access::Target(split, &space_a, 0x10'0800, 4) == 10);
    REQUIRE(Access::Target(split, &space_a, 0x30'0000, 4) == 0);
    // Small reads go by the word: the word next to the release doesn't wait for it.
    REQUIRE(Access::Target(split, &space_a, 0x20'0010, 4) == 20);
    REQUIRE(Access::Target(split, &space_a, 0x20'0014, 4) == 0);
    // Another address space may map the same memory: it waits for every recorded write.
    REQUIRE(Access::Target(split, &space_b, 0x30'0000, 4) == 20);
    // Once the back consumed past them, nothing is waited for.
    Access::SetPositions(split, 30, 25);
    REQUIRE(Access::Target(split, &space_a, 0x10'0800, 4) <= 25);
}

TEST_CASE("Maxwell3D's split front sends plain draw runs as one entry the back replays",
          "[video_core][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    constexpr u32 IndexAddressLow = MAXWELL3D_REG_INDEX(index_buffer.start_addr_low);
    constexpr u32 IndexFirst = MAXWELL3D_REG_INDEX(index_buffer.first);
    constexpr u32 IndexCount = MAXWELL3D_REG_INDEX(index_buffer.count);
    constexpr u32 BaseVertex = MAXWELL3D_REG_INDEX(global_base_vertex_index);
    constexpr u32 DepthWrite = MAXWELL3D_REG_INDEX(depth_write_enabled);
    struct Write {
        u32 method;
        u32 value;
    };
    // Six indexed draws with new draw parameters and constant buffer data before each; a depth
    // write (pipeline state) after the fourth ends its run.
    std::vector<Write> stream;
    for (u32 draw = 0; draw < 6; ++draw) {
        stream.push_back({BaseVertex, 7 + draw * 3});
        stream.push_back({IndexAddressLow, 0x1000 + draw * 0x100});
        stream.push_back({CbOffset, 0x40});
        stream.push_back({CbData, 0x100 + draw});
        stream.push_back({CbData + 1, 0x200 + draw});
        stream.push_back({DrawBegin, 4}); // Triangles, instance id First
        stream.push_back({IndexFirst, 1 + draw});
        stream.push_back({IndexCount, 3 + draw});
        stream.push_back({DrawEnd, 0});
        if (draw == 3) {
            stream.push_back({DepthWrite, 1});
        }
    }
    for (const Write& write : stream) {
        f.fast.FrontCallMethod(f.system, split, write.method, write.value, true);
        f.slow.CallMethod(f.system, write.method, write.value, true);
    }
    split.RunPendingFlush();
    const u64 end = Access::End(split);
    u64 position = 0;
    u32 entries = 0;
    while (position < end) {
        position = Access::Execute(split, position, end);
        ++entries;
    }
    REQUIRE(position == end);
    // One entry per method would be 9 per draw. With the runs: the engine, the first draw's
    // held state and data, a run of four draws, the state after it, the data and a run of two.
    CHECK(entries <= 8);
    REQUIRE(f.fast.regs.reg_array == f.slow.regs.reg_array);
    REQUIRE(f.fast.shadow_state.reg_array == f.slow.shadow_state.reg_array);
    const auto& a = f.fast.draw_manager.draw_state;
    const auto& b = f.slow.draw_manager.draw_state;
    CHECK(a.draw_mode == b.draw_mode);
    CHECK(a.draw_indexed == b.draw_indexed);
    CHECK(a.base_index == b.base_index);
    CHECK(a.base_instance == b.base_instance);
    CHECK(std::memcmp(&a.index_buffer, &b.index_buffer, sizeof(a.index_buffer)) == 0);
}

namespace {

using QmdDescriptor = Tegra::NativeQmdScheduler::Descriptor;

// What FrontQmdView told the split about a launch's releases.
struct QmdNote {
    enum class Kind { Write, Release, Forget } kind;
    GPUVAddr address;
    u64 value; ///< The size of a write, the value of a release
    bool at_least;
    bool operator==(const QmdNote&) const = default;
};

// Guest memory as the back finds it, and the split's answers (FrontQmdView's Memory).
struct QmdTestMemory {
    std::map<GPUVAddr, QmdDescriptor> qmds;
    std::map<GPUVAddr, u64> targets; ///< Per QMD: the position of the last write sent there
    std::map<GPUVAddr, u32> words;   ///< The words the front knows (GpuSplit::SemaphoreWordAt)
    std::vector<QmdNote> notes;
};

struct QmdTestView {
    QmdTestMemory* state;

    [[nodiscard]] bool IsMapped(GPUVAddr address, u64) const {
        return address < 0x1000'0000;
    }
    void Read(GPUVAddr address, void* data, u64 size) const {
        QmdDescriptor qmd{};
        if (const auto it = state->qmds.find(address); it != state->qmds.end()) {
            qmd = it->second;
        }
        std::memcpy(data, qmd.data(), size);
    }
    [[nodiscard]] std::optional<u64> Device(GPUVAddr address) const {
        return address;
    }
    [[nodiscard]] bool GpuWritten(u64, u64) const {
        return false;
    }
    [[nodiscard]] u64 WritesTarget(const VideoCommon::GpuSplit&, GPUVAddr address, u64) const {
        const auto it = state->targets.find(address);
        return it == state->targets.end() ? 0 : it->second;
    }
    [[nodiscard]] std::optional<u32> WordAt(VideoCommon::GpuSplit&, GPUVAddr address) const {
        const auto it = state->words.find(address);
        return it == state->words.end() ? std::nullopt : std::optional<u32>{it->second};
    }
    void NoteWrite(VideoCommon::GpuSplit&, GPUVAddr address, u64 size) const {
        state->notes.push_back({QmdNote::Kind::Write, address, size, false});
    }
    void NoteRelease(VideoCommon::GpuSplit&, GPUVAddr address, u32 value, bool at_least) const {
        state->notes.push_back({QmdNote::Kind::Release, address, value, at_least});
    }
    void ForgetRelease(VideoCommon::GpuSplit&, GPUVAddr address) const {
        state->notes.push_back({QmdNote::Kind::Forget, address, 0, false});
    }
};

constexpr u32 QmdEnable = 1U << 23;
constexpr u32 QmdOneWord = 1U << 30;
constexpr u32 QmdMaximum = 1U << 19;
constexpr u32 QmdReduction = 1U << 26;

QmdDescriptor MakeTestQmd(u32 dependencies) {
    QmdDescriptor qmd{};
    qmd[18] = 0x30;
    qmd[4] = dependencies << 14;
    return qmd;
}

void SetTestRelease(QmdDescriptor& qmd, u32 base, u32 address, u32 flags, u32 payload) {
    qmd[base] = address;
    qmd[base + 1] = flags;
    qmd[base + 2] = payload;
}

// The back's scheduling of the same signal, on the shared table: what it releases.
std::vector<std::pair<GPUVAddr, u32>> BackSignal(Tegra::NativeQmdScheduler& back,
                                                 const QmdTestMemory& memory, GPUVAddr address,
                                                 u32 argument) {
    std::vector<std::pair<GPUVAddr, u32>> released;
    const auto error = back.Signal(
        address, argument,
        [&](u64 qmd_address, QmdDescriptor& descriptor) {
            const auto it = memory.qmds.find(qmd_address);
            descriptor = it != memory.qmds.end() ? it->second : QmdDescriptor{};
            return true;
        },
        [&](u64 destination, u32 payload, u32) {
            released.emplace_back(destination, payload);
            return true;
        },
        [](const QmdDescriptor&) { return true; });
    REQUIRE(error.empty());
    return released;
}

} // Anonymous namespace

TEST_CASE("The split front's QMD view notes what launches release as the back runs them",
          "[video_core][gpu_split][front_qmd]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    VideoCommon::GpuSplit split;
    Access::SetBack(split, 10, 10);
    QmdTestMemory memory;
    const QmdTestView view_memory{&memory};
    constexpr GPUVAddr qmd_address = 0x7'0000;
    auto qmd = MakeTestQmd(1);
    SetTestRelease(qmd, 24, 0x5000, QmdEnable | QmdOneWord, 7);
    SetTestRelease(qmd, 28, 0x5010, QmdEnable | QmdOneWord | QmdMaximum, 3);
    SetTestRelease(qmd, 52, 0x5020, QmdEnable | QmdOneWord | QmdReduction, 1); // Add 1
    memory.qmds[qmd_address] = qmd;
    memory.words[0x5010] = 5;

    Tegra::NativeQmdScheduler back{Tegra::NativeQmdScheduler::OwnTable{}};
    VideoCommon::FrontQmdView view;
    view.TakeOver(back);
    // Loaded, it waits for its dependency: nothing released, on either side.
    REQUIRE(view.Signal(split, view_memory, qmd_address, 3, nullptr));
    REQUIRE(BackSignal(back, memory, qmd_address, 3).empty());
    REQUIRE(memory.notes.empty());
    // The decrement runs it: the front notes each release as the back runs it.
    REQUIRE(view.Signal(split, view_memory, qmd_address, 7, nullptr));
    const auto released = BackSignal(back, memory, qmd_address, 7);
    REQUIRE(released ==
            std::vector<std::pair<GPUVAddr, u32>>{{0x5000, 7}, {0x5010, 3}, {0x5020, 1}});
    REQUIRE(memory.notes == std::vector<QmdNote>{
                                {QmdNote::Kind::Write, 0x5000, 4, false},
                                {QmdNote::Kind::Release, 0x5000, 7, false},
                                // A maximum with a word the front knows: the maximum.
                                {QmdNote::Kind::Write, 0x5010, 4, false},
                                {QmdNote::Kind::Release, 0x5010, 5, false},
                                // A reduction of a word the front doesn't know: unknown.
                                {QmdNote::Kind::Write, 0x5020, 4, false},
                                {QmdNote::Kind::Forget, 0x5020, 0, false},
                            });
    // Again, with the reduction's word known and the maximum's not: the sum, and the payload
    // as a lower bound.
    memory.notes.clear();
    memory.words.erase(0x5010);
    memory.words[0x5020] = 41;
    REQUIRE(view.Signal(split, view_memory, qmd_address, 3, nullptr));
    REQUIRE(view.Signal(split, view_memory, qmd_address, 7, nullptr));
    REQUIRE(memory.notes[3] == QmdNote{QmdNote::Kind::Release, 0x5010, 3, true});
    REQUIRE(memory.notes[5] == QmdNote{QmdNote::Kind::Release, 0x5020, 42, false});
}

TEST_CASE("The split front's QMD view reads a QMD only as the back will find it",
          "[video_core][gpu_split][front_qmd]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    VideoCommon::GpuSplit split;
    QmdTestMemory memory;
    const QmdTestView view_memory{&memory};
    constexpr GPUVAddr loaded = 0x7'0000;
    constexpr GPUVAddr launched = 0x7'0100;
    auto first = MakeTestQmd(1);
    SetTestRelease(first, 24, 0x5000, QmdEnable | QmdOneWord, 1);
    auto second = MakeTestQmd(0);
    SetTestRelease(second, 24, 0x5000, QmdEnable | QmdOneWord, 2);
    memory.qmds[loaded] = first;
    memory.qmds[launched] = second;

    Tegra::NativeQmdScheduler back{Tegra::NativeQmdScheduler::OwnTable{}};
    VideoCommon::FrontQmdView view;
    view.TakeOver(back);
    Access::SetBack(split, 20, 10);
    // The QMD's own inline words, written by the back right before it runs the signal.
    REQUIRE(view.Signal(split, view_memory, launched, 3, &second));
    // A QMD written by work the back ran: memory holds it.
    memory.targets[loaded] = 8;
    REQUIRE(view.Signal(split, view_memory, loaded, 3, nullptr));
    // A queued inline write of it, the last write sent there: the front reads its words.
    auto patched = first;
    patched[26] = 9; // The release's payload
    Access::SetBack(split, 30, 10);
    view.NoteInlineWrite(split, loaded, patched);
    memory.targets[loaded] = 30;
    Access::SetBack(split, 40, 10);
    memory.notes.clear();
    REQUIRE(view.Signal(split, view_memory, loaded, 7, nullptr));
    REQUIRE(memory.notes[1] == QmdNote{QmdNote::Kind::Release, 0x5000, 9, false});
    // Another write still queued there: the front can't read it and loses the table.
    memory.targets[loaded] = 35;
    REQUIRE_FALSE(view.Signal(split, view_memory, loaded, 3, nullptr));
    REQUIRE_FALSE(view.Synced());
    REQUIRE_FALSE(view.Signal(split, view_memory, launched, 3, &second));
    // A write sent after the signal: the back reads what was before it. Lost as well.
    // Once the back ran everything sent, the view takes the back's table over.
    view.MaybeTakeOver(split);
    REQUIRE_FALSE(view.Synced());
    Access::SetBack(split, 40, 40);
    view.MaybeTakeOver(split);
    REQUIRE(view.Synced());
}

TEST_CASE("GpuSplit holds entries from an idle back while the front notes them",
          "[video_core][gpu_split][front_qmd]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    Access::SetBackIdle(split, true);
    // An idle back gets each entry at once.
    split.Defer([] {});
    REQUIRE(Access::Published(split) == Access::End(split));
    {
        const VideoCommon::GpuSplit::PublishHold hold{split, true};
        split.Defer([] {});
        {
            // A nested hold that holds nothing keeps the outer one.
            const VideoCommon::GpuSplit::PublishHold inner{split, false};
            split.Defer([] {});
        }
        REQUIRE(Access::Published(split) < Access::End(split));
    }
    // What Commit would have published goes once the hold ends.
    REQUIRE(Access::Published(split) == Access::End(split));
    {
        // A wait inside a hold still publishes.
        const VideoCommon::GpuSplit::PublishHold hold{split, true};
        split.Defer([] {});
        split.Publish();
        REQUIRE(Access::Published(split) == Access::End(split));
    }
}

TEST_CASE("GpuSplit narrow drains tell the 256-byte granules of a page apart for small writes",
          "[video_core][gpu_split]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    VideoCommon::GpuSplit split;
    Access::InitializeNarrow(split);
    int space = 0;
    // An inline QMD at position 10 and one next to it at 20, then a page-granular upload on
    // another page at 30.
    Access::SetPositions(split, 10, 0);
    split.NoteGranuleWrite(&space, 0x10'0000, 0x100);
    Access::SetPositions(split, 20, 0);
    split.NoteGranuleWrite(&space, 0x10'0100, 0x100);
    Access::SetPositions(split, 30, 0);
    split.NoteWrite(&space, 0x20'0000, 0x100);
    // A QMD read waits for its own granule's writes only.
    REQUIRE(Access::Target(split, &space, 0x10'0000, 0x100) == 10);
    REQUIRE(Access::Target(split, &space, 0x10'0100, 0x100) == 20);
    REQUIRE(Access::Target(split, &space, 0x10'0200, 0x100) == 0);
    // A read over both waits for both, a page-granular write for every read of its page.
    REQUIRE(Access::Target(split, &space, 0x10'00f0, 0x20) == 20);
    REQUIRE(Access::Target(split, &space, 0x20'0800, 4) == 30);
    // A larger read waits for every write on its pages.
    REQUIRE(Access::Target(split, &space, 0x10'0000, 0x1000) == 20);
}
