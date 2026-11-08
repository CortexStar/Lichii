// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/upload_batch.h"

namespace {

using Vulkan::UploadBatch;

VkBuffer Handle(u64 value) {
    return std::bit_cast<VkBuffer>(value);
}

VkBufferCopy Copy(VkDeviceSize src, VkDeviceSize dst, VkDeviceSize size) {
    return VkBufferCopy{.srcOffset = src, .dstOffset = dst, .size = size};
}

} // namespace

TEST_CASE("Upload batches keep apart copies that write the same bytes", "[video_core]") {
    UploadBatch batch;
    const VkBuffer staging = Handle(1);
    const VkBuffer a = Handle(2);
    const VkBuffer b = Handle(3);
    const std::array first{Copy(0, 0x100, 0x40)};
    REQUIRE(batch.CanAdd(staging, a, first));
    batch.Add(staging, a, first);
    // Adjacent bytes and other buffers join; overlapping bytes of the same buffer don't.
    REQUIRE(batch.CanAdd(staging, a, std::array{Copy(0x40, 0x140, 0x40)}));
    REQUIRE(batch.CanAdd(staging, b, std::array{Copy(0x40, 0x100, 0x40)}));
    REQUIRE_FALSE(batch.CanAdd(staging, a, std::array{Copy(0x40, 0x13f, 0x10)}));
    REQUIRE_FALSE(batch.CanAdd(staging, a, std::array{Copy(0x40, 0xc0, 0x41)}));
}

TEST_CASE("Upload batches keep apart copies that read a buffer another writes", "[video_core]") {
    UploadBatch batch;
    const VkBuffer staging = Handle(1);
    const VkBuffer a = Handle(2);
    batch.Add(staging, a, std::array{Copy(0, 0, 0x10)});
    // Reading a (written by the batch), or writing the staging buffer (read by it).
    REQUIRE_FALSE(batch.CanAdd(a, Handle(3), std::array{Copy(0x100, 0, 0x10)}));
    REQUIRE_FALSE(batch.CanAdd(Handle(3), staging, std::array{Copy(0, 0x100, 0x10)}));
}

TEST_CASE("Upload batches group runs of copies and stop at their size", "[video_core]") {
    UploadBatch batch;
    const VkBuffer staging = Handle(1);
    const VkBuffer a = Handle(2);
    const VkBuffer b = Handle(3);
    batch.Add(staging, a, std::array{Copy(0, 0, 4)});
    batch.Add(staging, a, std::array{Copy(4, 4, 4)});
    batch.Add(staging, b, std::array{Copy(8, 0, 4)});
    batch.Add(staging, a, std::array{Copy(12, 8, 4)});
    REQUIRE(batch.copies.size() == 4);
    REQUIRE(batch.groups.size() == 3);
    REQUIRE(batch.groups[0].first == 0);
    REQUIRE(batch.groups[0].count == 2);
    REQUIRE(batch.groups[1].dst == b);
    REQUIRE(batch.groups[2].first == 3);
    for (VkDeviceSize i = batch.copies.size(); i < UploadBatch::MaxCopies; ++i) {
        REQUIRE(batch.CanAdd(staging, b, std::array{Copy(0, 0x100 + i * 4, 4)}));
        batch.Add(staging, b, std::array{Copy(0, 0x100 + i * 4, 4)});
    }
    REQUIRE_FALSE(batch.CanAdd(staging, b, std::array{Copy(0, 0x1000, 4)}));
}
