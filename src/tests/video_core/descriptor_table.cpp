// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <catch2/catch_test_macros.hpp>
#include "common/container/unordered_map.h"
#include "common/slot_vector.h"
#include "video_core/texture_cache/descriptor_table.h"
#include "video_core/textures/texture.h"

namespace {
struct DescriptorMemory {
    std::array<std::array<u64, 128>, 2> banks{};

    // DescriptorTable::Read prefers a single-page host pointer when the memory manager offers one;
    // these tests are about the invalidation bookkeeping on the generic read path.
    const u8* GetPointer(GPUVAddr) const {
        return nullptr;
    }

    void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
        const auto bank = (address - 0x1000) / 0x1000;
        const auto offset = (address - 0x1000) % 0x1000;
        std::memcpy(output, reinterpret_cast<const u8*>(banks[bank].data()) + offset, size);
    }
};
} // namespace

TEST_CASE("Descriptor invalidation matches eager refresh across mutations", "[descriptor_table]") {
    DescriptorMemory memory;
    VideoCommon::DescriptorTable<u64> table;
    REQUIRE(table.Synchronize(0x1000, 127));
    std::array<bool, 128> seen{};
    std::array<u64, 128> previous{};
    std::mt19937 random{0x5080};
    unsigned bank{};
    u32 limit = 127;
    for (unsigned i = 0; i < 40000; ++i) {
        switch (random() % 5) {
        case 0:
            table.Invalidate();
            seen.fill(false);
            break;
        case 1:
            memory.banks[random() % 2][random() % 128] = random();
            break;
        case 2: {
            const auto next_bank = random() % 2;
            const u32 next_limit = random() % 2 ? 63 : 127;
            const bool changed = next_bank != bank || next_limit != limit;
            REQUIRE(table.Synchronize(0x1000 + next_bank * 0x1000, next_limit) == changed);
            if (changed) {
                seen.fill(false);
            }
            bank = next_bank;
            limit = next_limit;
            break;
        }
        default: {
            const u32 index = random() % (limit + 1);
            const u64 expected = memory.banks[bank][index];
            const bool changed = !seen[index] || previous[index] != expected;
            const auto actual = table.Read(memory, index);
            REQUIRE(actual.first == expected);
            REQUIRE(actual.second == changed);
            seen[index] = true;
            previous[index] = expected;
        }
        }
    }
}

TEST_CASE("Repeated image deletion cannot reuse stale descriptor view IDs", "[descriptor_table]") {
    DescriptorMemory memory;
    std::array<VideoCommon::DescriptorTable<u64>, 5> channels;
    std::array<std::array<unsigned, 128>, 5> view_ids{};
    unsigned live_generation = 1;
    const auto visit = [&](size_t channel, u32 index) {
        if (channels[channel].Read(memory, index).second) {
            view_ids[channel][index] = live_generation;
        }
        return view_ids[channel][index];
    };
    for (size_t channel = 0; channel < channels.size(); ++channel) {
        REQUIRE(channels[channel].Synchronize(0x1000, 127));
        for (u32 index = 0; index < 128; ++index) {
            REQUIRE(visit(channel, index) == live_generation);
        }
    }
    for (unsigned generation = 0; generation < 32; ++generation) {
        ++live_generation;
        for (unsigned deletion = 0; deletion < 50; ++deletion) {
            for (auto& table : channels) {
                table.Invalidate();
            }
        }
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            for (u32 index = 0; index < 128; ++index) {
                REQUIRE(visit(channel, index) == live_generation);
                REQUIRE_FALSE(channels[channel].Read(memory, index).second);
            }
        }
    }
}

TEST_CASE("Descriptor limits clear exposed slots after shrink and growth", "[descriptor_table]") {
    struct AddressMemory {
        const u8* GetPointer(GPUVAddr) const {
            return nullptr;
        }

        void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
            std::memcpy(output, &address, size);
        }
    } memory;
    constexpr std::array<u32, 9> limits{0, 63, 64, 127, 128, 0x7ffff, 0x80000, 0x80001, 0x100000};
    for (const u32 limit : limits) {
        VideoCommon::DescriptorTable<u64> table;
        REQUIRE(table.Synchronize(0x1000, limit));
        CHECK(table.Read(memory, limit).second);
        CHECK_FALSE(table.Read(memory, limit).second);
        table.Refresh(0x1000, 0);
        table.Invalidate();
        CHECK(table.Read(memory, 0).second);
        CHECK_FALSE(table.Read(memory, 0).second);
        table.Refresh(0x1000, limit);
        CHECK(table.Read(memory, limit).second);
        CHECK_FALSE(table.Read(memory, limit).second);
        table.Invalidate();
        table.Invalidate();
        CHECK(table.Read(memory, limit).second);
        CHECK_FALSE(table.Read(memory, limit).second);
    }
    for (const u32 limit :
         {std::numeric_limits<u32>::max(), std::numeric_limits<u32>::max() - 0x1000}) {
        VideoCommon::DescriptorTable<u64> table;
        table.Refresh(0x1000, limit);
        CHECK(table.Read(memory, 0).second);
        table.Invalidate();
        CHECK(table.Read(memory, 0).second);
        CHECK_FALSE(table.Read(memory, 0).second);
    }
}

TEST_CASE("Descriptor invalidation CPU benchmark", "[.][cache_bench]") {
    DescriptorMemory memory;
    const auto measure = [&](bool deferred) {
        std::array<VideoCommon::DescriptorTable<u64>, 5> tables;
        for (auto& table : tables) {
            REQUIRE(table.Synchronize(0x1000, 127));
        }
        void (*volatile invalidate)(VideoCommon::DescriptorTable<u64>&) =
            deferred ? +[](VideoCommon::DescriptorTable<u64>& table) { table.Invalidate(); }
                     : +[](VideoCommon::DescriptorTable<u64>& table) {
                           std::ranges::fill(table.read_descriptors, 0);
                       };
        size_t changed{};
        const auto start = std::chrono::steady_clock::now();
        for (unsigned batch = 0; batch < 2000; ++batch) {
            for (unsigned deletion = 0; deletion < 50; ++deletion) {
                for (auto& table : tables) {
                    invalidate(table);
                }
            }
            for (auto& table : tables) {
                changed += table.Read(memory, 0).second;
            }
        }
        const auto ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        REQUIRE(changed == 10000);
        return ms;
    };
    const double eager = measure(false);
    const double deferred = measure(true);
    std::cout << "Descriptor bitmap invalidation: eager=" << eager << " ms deferred=" << deferred
              << " ms (CPU only, not gameplay FPS)\n";
}

TEST_CASE("Bound descriptor IDs survive hits but not mutations or invalidation",
          "[descriptor_table]") {
    DescriptorMemory memory;
    using Id = Common::SlotId;
    std::array<VideoCommon::DescriptorTable<u64, Id>, 4> tables;
    u32 generation = 1;
    const auto visit = [&](size_t table_index, u32 index) {
        auto& table = tables[table_index];
        const auto [descriptor, changed] = table.Read(memory, index);
        if (changed) {
            table.bound_ids[index] =
                descriptor == 0 ? Id{0} : Id{generation * 1024 + u32(table_index) * 128 + index};
        }
        return table.bound_ids[index];
    };
    for (auto& bank : memory.banks) {
        bank.fill(1);
    }
    for (auto& table : tables) {
        REQUIRE(table.Synchronize(0x1000, 127));
    }
    for (size_t t = 0; t < tables.size(); ++t) {
        REQUIRE(visit(t, 127) == Id{1024 + u32(t) * 128 + 127});
    }
    ++generation;
    for (size_t t = 0; t < tables.size(); ++t) {
        REQUIRE_FALSE(tables[t].Synchronize(0x1000, 127));
        REQUIRE(visit(t, 127) == Id{1024 + u32(t) * 128 + 127});
    }
    for (unsigned deletion = 0; deletion < 50; ++deletion) {
        for (auto& table : tables) {
            table.Invalidate();
        }
    }
    for (size_t t = 0; t < tables.size(); ++t) {
        REQUIRE(visit(t, 127) == Id{2048 + u32(t) * 128 + 127});
    }
    memory.banks[0][127] = 0;
    REQUIRE(visit(0, 127) == Id{0});
    REQUIRE(visit(0, 127) == Id{0});
    memory.banks[0][127] = 2;
    ++generation;
    REQUIRE(visit(0, 127) == Id{3072 + 127});
    REQUIRE(tables[1].bound_ids[127] == Id{2048 + 128 + 127});
    REQUIRE(visit(1, 127) == Id{3072 + 128 + 127});
    REQUIRE(tables[0].Synchronize(0x1000, 0));
    ++generation;
    REQUIRE(visit(0, 0) == Id{4096});
    REQUIRE(tables[0].Synchronize(0x1000, 127));
    REQUIRE(visit(0, 127) == Id{4096 + 127});
    REQUIRE(tables[0].Synchronize(0x2000, 127));
    ++generation;
    REQUIRE(visit(0, 127) == Id{5120 + 127});
}

TEST_CASE("Bound descriptor IDs match the previous tagged lookup across channels",
          "[descriptor_table]") {
    DescriptorMemory memory;
    using Id = Common::SlotId;
    std::array<std::array<VideoCommon::DescriptorTable<u64, Id>, 2>, 3> channels;
    std::array<Common::unordered_map<u32, Id>, 3> old_ids;
    std::mt19937 random{0x256};
    u32 generation = 1;
    for (auto& channel : channels) {
        for (auto& table : channel) {
            REQUIRE(table.Synchronize(0x1000, 127));
        }
    }
    for (unsigned operation = 0; operation < 40000; ++operation) {
        const size_t channel = random() % channels.size();
        const size_t compute = random() % 2;
        const u32 index = random() % 128;
        if (operation % 31 == 0) {
            memory.banks[0][index] = random();
        }
        if (operation % 257 == 0) {
            ++generation;
            for (auto& tables : channels) {
                for (auto& table : tables) {
                    table.Invalidate();
                }
            }
        }
        auto& table = channels[channel][compute];
        const u32 key = index | (compute ? Id::TAGGED_VALUE : 0);
        const auto [descriptor, changed] = table.Read(memory, index);
        if (changed) {
            const Id id{descriptor == 0 ? 0U : generation * 1024 + index};
            table.bound_ids[index] = id;
            old_ids[channel].insert_or_assign(key, id);
        }
        REQUIRE(table.bound_ids[index] == old_ids[channel].at(key));
    }
}

TEST_CASE("Bound descriptor IDs refresh after descriptor capacity changes", "[descriptor_table]") {
    struct Memory {
        const u8* GetPointer(GPUVAddr) const {
            return nullptr;
        }

        void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
            std::memcpy(output, &address, size);
        }
    } memory;
    VideoCommon::DescriptorTable<u64, Common::SlotId> table;
    for (const u32 limit : {0U, 63U, 64U, 127U, 0x7ffffU, 0x80000U, 0x80001U}) {
        REQUIRE(table.Synchronize(0x1000, limit));
        REQUIRE(table.Read(memory, limit).second);
        table.bound_ids[limit] = Common::SlotId{limit + 1};
        REQUIRE_FALSE(table.Read(memory, limit).second);
        REQUIRE(table.bound_ids[limit] == Common::SlotId{limit + 1});
        table.Refresh(0x1000, 0);
        table.Refresh(0x1000, limit);
        REQUIRE(table.Read(memory, limit).second);
        REQUIRE(table.bound_ids[limit] == Common::SlotId{0});
        table.bound_ids[limit] = Common::SlotId{limit + 2};
        table.Invalidate();
        REQUIRE(table.Read(memory, limit).second);
        REQUIRE(table.bound_ids[limit] == Common::SlotId{0});
    }
}

TEST_CASE("Descriptor Debug poisoning touches only the current addressable range",
          "[descriptor_table]") {
    DescriptorMemory memory;
    using Id = Common::SlotId;
    constexpr Id poison{0xfffffffe};
    constexpr Id untouched{0x1234};
    VideoCommon::DescriptorTable<u64, Id> table;
    REQUIRE(table.LiveBoundIds().empty());
    REQUIRE(table.bound_ids.capacity() == 0);
    std::ranges::fill(table.LiveBoundIds(), poison);
    REQUIRE(table.bound_ids.capacity() == 0);

    REQUIRE(table.Synchronize(0x1000, 127));
    REQUIRE(table.bound_ids.size() == 524289);
    REQUIRE(table.LiveBoundIds().size() == 128);
    std::ranges::fill(table.bound_ids, untouched);
    REQUIRE(table.Read(memory, 127).second);
    table.bound_ids[127] = untouched;
    const auto* data = table.bound_ids.data();
    const auto capacity = table.bound_ids.capacity();
    std::ranges::fill(table.LiveBoundIds(), poison);
    table.Invalidate();
    REQUIRE(table.invalidated);
    REQUIRE(table.bound_ids[0] == poison);
    REQUIRE(table.bound_ids[127] == poison);
    REQUIRE(table.bound_ids[128] == untouched);
    REQUIRE(table.bound_ids.back() == untouched);
    REQUIRE(table.bound_ids.data() == data);
    REQUIRE(table.bound_ids.capacity() == capacity);
    REQUIRE(table.Read(memory, 127).second);
    REQUIRE_FALSE(table.invalidated);
    REQUIRE(table.bound_ids[127] == Id{0});
    REQUIRE(table.bound_ids[128] == untouched);

    REQUIRE(table.Synchronize(0x1000, 0));
    REQUIRE(table.LiveBoundIds().size() == 1);
    table.bound_ids[1] = untouched;
    std::ranges::fill(table.LiveBoundIds(), poison);
    table.Invalidate();
    REQUIRE(table.bound_ids[0] == poison);
    REQUIRE(table.bound_ids[1] == untouched);
    REQUIRE(table.Read(memory, 0).second);
    REQUIRE(table.bound_ids[0] == Id{0});
    REQUIRE(table.bound_ids[1] == untouched);

    REQUIRE(table.Synchronize(0x1000, 127));
    REQUIRE(table.LiveBoundIds().size() == 128);
    REQUIRE(table.bound_ids[1] == Id{0});
    REQUIRE(table.Read(memory, 1).second);
    REQUIRE_FALSE(table.Read(memory, 1).second);
    std::ranges::fill(table.LiveBoundIds(), poison);
    table.Invalidate();
    REQUIRE(table.bound_ids[127] == poison);
    REQUIRE(table.bound_ids[128] == untouched);
    REQUIRE(table.Read(memory, 127).second);
}

TEST_CASE("Descriptor ID lookup CPU benchmark", "[.][descriptor_lookup_bench]") {
    using Descriptor = Tegra::Texture::TICEntry;
    using Id = Common::SlotId;
    struct Memory {
        std::vector<Descriptor> descriptors;

        const u8* GetPointer(GPUVAddr address) const {
            return reinterpret_cast<const u8*>(descriptors.data()) + address - 0x1000;
        }

        void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
            std::memcpy(output, GetPointer(address), size);
        }
    };
    struct LookupBenchmarkResult {
        double ns_per_lookup;
        u64 checksum;
    };
    constexpr size_t operations = 2'000'000;
    const auto measure = [&]<bool flat>(u32 size, bool mutate, const std::vector<u32>& indices) {
        Memory memory{std::vector<Descriptor>(size)};
        for (u32 i = 0; i < size; ++i) {
            memory.descriptors[i].raw = {i, i + 1, i + 2, i + 3};
        }
        std::array<VideoCommon::DescriptorTable<Descriptor, Id>, 2> tables;
        Common::unordered_map<u32, Id> old_ids;
        for (auto& table : tables) {
            REQUIRE(table.Synchronize(0x1000, size - 1));
        }
        const auto lookup = [&](u32 index, u32 compute) {
            auto& table = tables[compute];
            const auto [descriptor, changed] = table.Read(memory, index);
            const u32 key = index | (compute ? Id::TAGGED_VALUE : 0);
            if (changed) {
                const Id id{static_cast<u32>(descriptor.raw[0]) + 1};
                if constexpr (flat) {
                    table.bound_ids[index] = id;
                } else {
                    old_ids.insert_or_assign(key, id);
                }
                return id.Value();
            }
            if constexpr (flat) {
                return table.bound_ids[index].Value();
            } else {
                return old_ids.find(key)->second.Value();
            }
        };
        for (u32 i = 0; i < size; ++i) {
            lookup(i, 0);
            lookup(i, 1);
        }
        u64 checksum{};
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < operations; ++i) {
            const u32 index = indices[i];
            if (mutate && i % 1024 == 0) {
                ++memory.descriptors[index].raw[0];
            }
            checksum += lookup(index, static_cast<u32>(i % 128 == 0));
        }
        const double ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                .count();
        return LookupBenchmarkResult{ns / operations, checksum};
    };
    for (const u32 size : {128U, 4096U, 16384U}) {
        std::mt19937 random{0x256};
        std::vector<u32> indices(operations);
        for (auto& index : indices) {
            index = random() % size;
        }
        for (const bool mutate : {false, true}) {
            std::array<double, 7> before{};
            std::array<double, 7> after{};
            for (size_t trial = 0; trial < before.size(); ++trial) {
                LookupBenchmarkResult old_result{}, new_result{};
                if (trial % 2 == 0) {
                    old_result = measure.template operator()<false>(size, mutate, indices);
                    new_result = measure.template operator()<true>(size, mutate, indices);
                } else {
                    new_result = measure.template operator()<true>(size, mutate, indices);
                    old_result = measure.template operator()<false>(size, mutate, indices);
                }
                REQUIRE(old_result.checksum == new_result.checksum);
                before[trial] = old_result.ns_per_lookup;
                after[trial] = new_result.ns_per_lookup;
            }
            std::ranges::sort(before);
            std::ranges::sort(after);
            std::cout << "Descriptor lookup entries=" << size << " mutate=" << mutate
                      << " before_ns=" << before[3] << " after_ns=" << after[3]
                      << " ratio=" << after[3] / before[3]
                      << " (median of 7, CPU only, not gameplay FPS)\n";
        }
    }
}

namespace {
/// Guest memory of a descriptor pool whose writes are reported per page, as the texture cache's
/// write watch reports them for armed pages.
struct WatchedMemory {
    std::vector<u64> words = std::vector<u64>(2048);

    const u8* GetPointer(GPUVAddr) const {
        return nullptr;
    }

    void ReadBlockUnsafe(GPUVAddr address, void* output, size_t size) const {
        std::memcpy(output, reinterpret_cast<const u8*>(words.data()) + (address - 0x1000), size);
    }
};
} // namespace

TEST_CASE("Resident descriptors always equal guest memory under watched writes",
          "[descriptor_table]") {
    using Table = VideoCommon::DescriptorTable<u64, Common::SlotId>;
    WatchedMemory memory;
    Table table;
    // Entries 0..1023 over pages 0..2: the base is not page aligned.
    constexpr GPUVAddr base = 0x1000 + 0x100;
    REQUIRE(table.Synchronize(base, 1023));
    REQUIRE(table.pages.size() == 3);
    std::mt19937 random{0x7e57};
    u64 frame{};
    u64 resident_hits{};
    for (unsigned i = 0; i < 400000; ++i) {
        const u32 index = random() % 1024;
        const u32 op = random() % 1024;
        if (op < 17) {
            // A write: armed pages report it, unarmed ones are not watched. One in 17 goes
            // anywhere, the others to the last page.
            const u32 target = op == 16 && random() % 64 == 0 ? index : 1000 + index % 24;
            memory.words[(base - 0x1000) / 8 + target] = random() % 4;
            const u32 page = table.PageOf(target);
            if (table.pages[page].armed) {
                table.NoteDisarmed(page, frame, true);
            }
        } else if (op < 33) {
            ++frame;
            table.ClearAllResident();
        } else if (op == 33) {
            table.Forget(index);
        } else if (op == 34) {
            table.Invalidate();
        } else if (table.IsResident(index)) {
            ++resident_hits;
            REQUIRE(table.descriptors[index] == memory.words[(base - 0x1000) / 8 + index]);
            REQUIRE(table.bound_ids[index] ==
                    Common::SlotId{static_cast<u32>(table.descriptors[index]) + 1});
        } else {
            const u32 page = table.PageOf(index);
            REQUIRE(table.InOnePage(index));
            // Armed before the read, as the texture cache does.
            const bool armed = table.pages[page].armed || table.MayArm(page, frame);
            if (armed && !table.pages[page].armed) {
                table.NoteArmed(page, page + 100, frame);
            }
            const auto [descriptor, changed] = table.Read(memory, index);
            if (changed) {
                table.bound_ids[index] = Common::SlotId{static_cast<u32>(descriptor) + 1};
            }
            REQUIRE(table.bound_ids[index] == Common::SlotId{static_cast<u32>(descriptor) + 1});
            if (armed) {
                table.SetResident(index);
            }
        }
    }
    CHECK(resident_hits > 10000);
}

TEST_CASE("Descriptor pool pages back off after writes", "[descriptor_table]") {
    using Table = VideoCommon::DescriptorTable<u64>;
    Table table;
    REQUIRE(table.Synchronize(0x2000, 1023));
    REQUIRE(table.pages.size() == 2);
    REQUIRE(table.PageOf(511) == 0);
    REQUIRE(table.PageOf(512) == 1);
    REQUIRE(table.PageAddress(1) == 0x3000);

    table.NoteArmed(0, 7, 10);
    REQUIRE(table.ArmedPages() == 1);
    REQUIRE_FALSE(table.MayArm(0, 10));
    table.SetResident(3);
    table.SetResident(600);
    table.NoteDisarmed(0, 10, true);
    CHECK_FALSE(table.IsResident(3));
    CHECK(table.IsResident(600));
    CHECK(table.ArmedPages() == 0);
    // First strike: one frame.
    CHECK_FALSE(table.MayArm(0, 10));
    CHECK(table.MayArm(0, 11));
    // Written right after each arm: 3, 7, 15, 31, 63 frames, then capped.
    u64 frame = 11;
    for (const u64 wait : {3U, 7U, 15U, 31U, 63U, 63U}) {
        table.NoteArmed(0, 7, frame);
        table.NoteDisarmed(0, frame, true);
        CHECK_FALSE(table.MayArm(0, frame + wait - 1));
        CHECK(table.MayArm(0, frame + wait));
        frame += wait;
    }
    // Quiet for QUIET_FRAMES: starts over at one frame.
    table.NoteArmed(0, 7, frame);
    table.NoteDisarmed(0, frame + Table::QUIET_FRAMES, true);
    CHECK(table.MayArm(0, frame + Table::QUIET_FRAMES + 1));
    // A disarm without a write (rebinding) leaves the back-off alone.
    table.NoteArmed(1, 8, frame);
    table.NoteDisarmed(1, frame, false);
    CHECK(table.MayArm(1, frame));
    CHECK(table.ArmedPages() == 0);
}

TEST_CASE("Descriptor entries crossing a page are never in one page", "[descriptor_table]") {
    VideoCommon::DescriptorTable<std::array<u64, 4>> table;
    REQUIRE(table.Synchronize(0x1000 + 0x10, 255));
    CHECK(table.InOnePage(0));
    CHECK_FALSE(table.InOnePage(127)); // bytes 0xFF0..0x100F
    CHECK(table.InOnePage(128));
    CHECK(table.PageOf(128) == 1);
    CHECK(table.pages.size() == 3);
    table.SetResident(127);
    table.Forget(127);
    CHECK_FALSE(table.IsResident(127));
}

TEST_CASE("Descriptor heap mirror sees every bound id change once", "[descriptor_table]") {
    using Id = Common::SlotId;
    VideoCommon::DescriptorTable<u64, Id> table;
    REQUIRE(table.Synchronize(0x1000, 255));
    REQUIRE(table.TakeMirrorReset());
    REQUIRE_FALSE(table.TakeMirrorReset());
    std::vector<Id> heap(256, Id{0});
    const auto drain = [&] {
        table.TakeMirrorUpdates([&](u32 index, Id id) {
            REQUIRE(heap[index] != id);
            heap[index] = id;
        });
    };
    WatchedMemory memory;
    std::mt19937 random{0x3141};
    for (unsigned i = 0; i < 100000; ++i) {
        const u32 index = random() % 256;
        switch (random() % 6) {
        case 0:
            table.Forget(index);
            break;
        case 1:
            drain();
            for (u32 entry = 0; entry < 256; ++entry) {
                if (((table.read_descriptors[entry / 64] >> (entry % 64)) & 1) != 0) {
                    REQUIRE(heap[entry] == table.bound_ids[entry]);
                }
            }
            break;
        case 2:
            memory.words[index] = random() % 3;
            break;
        default:
            if (table.Read(memory, index).second) {
                const u64 descriptor = table.descriptors[index];
                table.Bind(index, Id{static_cast<u32>(descriptor)});
            }
        }
        REQUIRE(table.MirrorMismatches() == 0);
    }
    // Rebinding the pool starts the mirror over.
    REQUIRE(table.Synchronize(0x1000, 127));
    REQUIRE(table.TakeMirrorReset());
    table.TakeMirrorUpdates([](u32, Id) { FAIL("no updates after a reset"); });
}
