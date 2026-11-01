// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "video_core/texture_cache/sparse_image.h"
#include "video_core/vulkan_common/sparse_binding_transaction.h"

using VideoCommon::ImageFlagBits;
using VideoCommon::LevelRect;
using VideoCommon::PrepareSparseImageWrite;

TEST_CASE("Sparse image writes wait for residency and initialize only new tiles",
          "[nx2_sparse_image]") {
    auto flags = ImageFlagBits::SparseTiles | ImageFlagBits::Unbacked | ImageFlagBits::Tracked;
    std::vector<LevelRect> pending;
    std::vector<char> calls;
    std::array<int, 2> contents{42, -1};
    const auto bind = [&](auto& rects) {
        calls.push_back('b');
        rects.push_back({0, 128, 0, 128, 64});
        return true;
    };
    const auto upload = [&](const auto& rects) {
        calls.push_back('u');
        REQUIRE(rects == std::vector<LevelRect>{{0, 128, 0, 128, 64}});
        contents[1] = 0;
    };
    REQUIRE(PrepareSparseImageWrite(flags, pending, bind, upload));
    calls.push_back('w');
    CHECK(calls == std::vector<char>{'b', 'u', 'w'});
    CHECK(contents == std::array{42, 0});
    CHECK(flags == ImageFlagBits::Tracked);
    CHECK(pending.empty());
    CHECK_FALSE(PrepareSparseImageWrite(flags, pending, bind, upload));
    CHECK(calls == std::vector<char>{'b', 'u', 'w'});
}

TEST_CASE("Failed sparse promotion never schedules its writer or clears residency flags",
          "[nx2_sparse_image]") {
    const auto original = ImageFlagBits::SparseTiles | ImageFlagBits::Unbacked;
    auto flags = original;
    std::vector<LevelRect> pending;
    bool uploaded = false;
    bool written = false;
    const auto write = [&] {
        PrepareSparseImageWrite(flags, pending, [](auto&) { return false; },
                                [&](const auto&) { uploaded = true; });
        written = true;
    };
    CHECK_THROWS_AS(write(), std::bad_alloc);
    CHECK_FALSE(uploaded);
    CHECK_FALSE(written);
    CHECK(flags == original);
    CHECK(pending.empty());
}

TEST_CASE("Sparse upload failures retain initialization work for the next attempt",
          "[nx2_sparse_image]") {
    const auto original = ImageFlagBits::SparseTiles | ImageFlagBits::Unbacked;
    auto flags = original;
    std::vector<LevelRect> pending;
    bool written = false;
    const auto write = [&] {
        PrepareSparseImageWrite(flags, pending, [](auto& rects) {
            rects.push_back({0, 128, 0, 128, 64});
            return true;
        }, [](const auto&) { throw std::runtime_error{"staging failure"}; });
        written = true;
    };
    CHECK_THROWS_AS(write(), std::runtime_error);
    CHECK_FALSE(written);
    CHECK(flags == original);
    REQUIRE(pending.size() == 1);
    CHECK(PrepareSparseImageWrite(flags, pending, [](auto&) { return true; },
                                 [](const auto& rects) { REQUIRE(rects.size() == 1); }));
    CHECK(pending.empty());
    CHECK(False(flags & ImageFlagBits::SparseTiles));
}

TEST_CASE("Sparse allocation transactions roll back partial tiles and tails", "[nx2_sparse_image]") {
    struct Allocator {
        size_t calls{};
        size_t freed{};
        size_t fail_at = 2;
        VmaAllocation AllocateSparseMemory(const VkMemoryRequirements&, VkDeviceMemory&,
                                            VkDeviceSize&) {
            ++calls;
            return calls == fail_at ? VK_NULL_HANDLE : reinterpret_cast<VmaAllocation>(calls);
        }
        void FreeSparseMemory(VmaAllocation allocation) {
            REQUIRE(allocation != VK_NULL_HANDLE);
            ++freed;
        }
    } allocator;
    VkDeviceSize bytes = 4096;
    VkDeviceMemory memory{};
    VkDeviceSize offset{};
    VmaAllocation tile{};
    VmaAllocation tail{};
    {
        Vulkan::SparseBindingTransaction transaction{allocator, bytes};
        CHECK(transaction.Allocate({4096, 4096, 1}, memory, offset, tile));
        CHECK_FALSE(transaction.Allocate({8192, 4096, 1}, memory, offset, tail));
    }
    CHECK(bytes == 4096);
    CHECK(tile == VK_NULL_HANDLE);
    CHECK(tail == VK_NULL_HANDLE);
    CHECK(allocator.freed == 1);
    allocator.fail_at = 0;
    {
        Vulkan::SparseBindingTransaction transaction{allocator, bytes};
        CHECK(transaction.Allocate({4096, 4096, 1}, memory, offset, tile));
        CHECK(transaction.Allocate({8192, 4096, 1}, memory, offset, tail));
    }
    CHECK(bytes == 4096);
    CHECK(tile == VK_NULL_HANDLE);
    CHECK(tail == VK_NULL_HANDLE);
    CHECK(allocator.freed == 3);
    {
        Vulkan::SparseBindingTransaction transaction{allocator, bytes};
        CHECK(transaction.Allocate({4096, 4096, 1}, memory, offset, tile));
        transaction.Commit();
    }
    CHECK(bytes == 8192);
    CHECK(tile != VK_NULL_HANDLE);
    CHECK(allocator.freed == 3);
}
