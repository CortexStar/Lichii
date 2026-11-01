// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"

namespace {

VideoCommon::ImageInfo MakeImage(u32 width, u32 height) {
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::R8_UNORM;
    info.size = {width, height, 1};
    info.resources = {1, 1};
    return info;
}

} // namespace

TEST_CASE("Sparse texture byte ranges select host tiles from guest GOB columns",
          "[nx2_sparse_image]") {
    const auto info = MakeImage(256, 128);
    const std::array<std::pair<u64, u64>, 2> ranges{{{0, 1}, {1024, 1}}};
    CHECK(VideoCommon::SparseTileRects(info, ranges, 128, 64, 1) ==
          std::vector<VideoCommon::LevelRect>{{0, 0, 0, 256, 64}});
    const std::array<std::pair<u64, u64>, 1> lower{{{16384, 1}}};
    const auto rects = VideoCommon::SparseTileRects(info, lower, 128, 64, 1);
    CHECK(rects == std::vector<VideoCommon::LevelRect>{{0, 0, 64, 128, 64}});
    CHECK(VideoCommon::UnswizzledRectsBytes(info, rects) == 128 * 64);
    CHECK(VideoCommon::SparseTileRects(info, {}, 128, 64, 1).empty());
}

TEST_CASE("Sparse texture edge tiles clip to the image width", "[nx2_sparse_image]") {
    const auto info = MakeImage(130, 65);
    const std::array<std::pair<u64, u64>, 1> edge{{{1024, 1}}};
    const auto rects = VideoCommon::SparseTileRects(info, edge, 128, 64, 1);
    CHECK(rects == std::vector<VideoCommon::LevelRect>{{0, 128, 0, 2, 64}});
    CHECK(VideoCommon::UnswizzledRectsBytes(info, rects) == 64 * 64);
    const std::array<std::pair<u64, u64>, 1> next_row{{{1536, 512}}};
    CHECK(VideoCommon::SparseTileRects(info, next_row, 128, 64, 1) ==
          std::vector<VideoCommon::LevelRect>{{0, 0, 0, 128, 64}});
}

TEST_CASE("Sparse mip tails upload whole levels only when touched", "[nx2_sparse_image]") {
    auto info = MakeImage(256, 128);
    info.resources.levels = 3;
    const auto offsets = VideoCommon::CalculateMipLevelOffsets(info);
    const std::array<std::pair<u64, u64>, 1> tail{{{offsets[2] + 1, 1}}};
    CHECK(VideoCommon::SparseTileRects(info, tail, 128, 64, 1) ==
          std::vector<VideoCommon::LevelRect>{{2, 0, 0, 64, 32}});
    const std::array<std::pair<u64, u64>, 1> boundary{{{offsets[1] - 1, 2}}};
    const auto rects = VideoCommon::SparseTileRects(info, boundary, 128, 64, 1);
    REQUIRE(rects.size() == 2);
    CHECK(rects.back() == VideoCommon::LevelRect{1, 0, 0, 128, 64});
}

TEST_CASE("Sparse rectangle uploads match the whole-image unswizzle bytes and regions",
          "[nx2_sparse_image][nx2_sparse_unswizzle]") {
    using VideoCore::Surface::PixelFormat;
    const auto format = GENERATE(PixelFormat::R8_UNORM, PixelFormat::R16_UNORM,
                                 PixelFormat::A8B8G8R8_UNORM, PixelFormat::R32G32_FLOAT,
                                 PixelFormat::R32G32B32A32_FLOAT);
    const auto block_height = GENERATE(0U, 2U, 4U);
    const auto spacing = GENERATE(0U, 1U);
    const bool native_tiling = GENERATE(false, true);
    CAPTURE(format, block_height, spacing, native_tiling);
    auto info = MakeImage(258, 137);
    info.format = format;
    info.block.height = block_height;
    info.tile_width_spacing = spacing;
    info.resources.levels = 5;
    const u32 bytes_per_pixel = VideoCore::Surface::BytesPerBlock(format);
    std::vector<u8> guest(VideoCommon::CalculateGuestSizeInBytes(info));
    u32 random = 0x91e10da5;
    for (auto& byte : guest) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<u8>(random);
    }
    std::vector<u8> whole(VideoCommon::CalculateUnswizzledSizeBytes(info));
    const auto whole_copies =
        VideoCommon::UnswizzleBlockLinearImage(info, guest, whole, native_tiling);
    REQUIRE(whole_copies.size() == 5);
    std::vector<VideoCommon::LevelRect> rects{{0, 128, 64, 128, 64}, {0, 256, 128, 2, 9}};
    for (s32 level = 1; level < info.resources.levels; ++level) {
        const auto& extent = whole_copies[level].image_extent;
        rects.push_back({level, 0, 0, extent.width, extent.height});
    }

    const auto output_size = VideoCommon::UnswizzledRectsBytes(info, rects);
    constexpr size_t guard_size = 32;
    std::vector<u8> guarded_output(output_size + 2 * guard_size, 0xa5);
    const auto output = std::span{guarded_output}.subspan(guard_size, output_size);
    constexpr GPUVAddr guest_base = 0x123450000;
    size_t reads = 0;
    const auto copies = VideoCommon::UnswizzleImageRects(
        [&](GPUVAddr address, std::span<u8> bytes) {
            REQUIRE(address >= guest_base);
            const auto offset = address - guest_base;
            REQUIRE(offset <= guest.size());
            REQUIRE(bytes.size() <= guest.size() - offset);
            std::memcpy(bytes.data(), guest.data() + offset, bytes.size());
            ++reads;
        },
        guest_base, info, rects, output, native_tiling);
    REQUIRE_FALSE(copies.empty());
    CHECK(reads == copies.size());
    CHECK(std::ranges::all_of(std::span{guarded_output}.first(guard_size),
                              [](u8 byte) { return byte == 0xa5; }));
    CHECK(std::ranges::all_of(std::span{guarded_output}.last(guard_size),
                              [](u8 byte) { return byte == 0xa5; }));

    std::vector<u8> actual(whole.size(), 0xcd);
    std::vector<u8> expected(whole.size(), 0xcd);
    std::vector<u8> actual_coverage(whole.size());
    std::vector<u8> expected_coverage(whole.size());
    for (const auto& rect : rects) {
        const auto& whole_copy = whole_copies[rect.level];
        for (u32 y = rect.y; y < rect.y + rect.height; ++y) {
            const size_t offset = whole_copy.buffer_offset +
                                  (size_t{y} * whole_copy.buffer_row_length + rect.x) *
                                      bytes_per_pixel;
            const size_t bytes = size_t{rect.width} * bytes_per_pixel;
            std::copy_n(whole.begin() + offset, bytes, expected.begin() + offset);
            std::fill_n(expected_coverage.begin() + offset, bytes, 1);
        }
    }
    size_t consumed_bytes = 0;
    for (const auto& copy : copies) {
        const auto level = copy.image_subresource.base_level;
        REQUIRE(level >= 0);
        REQUIRE(level < info.resources.levels);
        const auto& whole_copy = whole_copies[level];
        CHECK(copy.image_subresource.base_layer == 0);
        CHECK(copy.image_subresource.num_layers == 1);
        CHECK(copy.image_offset.z == 0);
        CHECK(copy.image_extent.depth == 1);
        REQUIRE(copy.image_offset.x >= 0);
        REQUIRE(copy.image_offset.y >= 0);
        REQUIRE(copy.image_offset.x + copy.image_extent.width <= whole_copy.image_extent.width);
        REQUIRE(copy.image_offset.y + copy.image_extent.height <= whole_copy.image_extent.height);
        REQUIRE(copy.buffer_row_length >= copy.image_extent.width);
        REQUIRE(copy.buffer_image_height >= copy.image_extent.height);
        CHECK(copy.buffer_size == size_t{copy.buffer_row_length} * copy.buffer_image_height *
                                      bytes_per_pixel);
        REQUIRE(copy.buffer_offset >= consumed_bytes);
        REQUIRE(copy.buffer_offset < consumed_bytes + copy.buffer_size);
        consumed_bytes += copy.buffer_size;
        for (u32 row = 0; row < copy.image_extent.height; ++row) {
            const size_t source = copy.buffer_offset +
                                  size_t{row} * copy.buffer_row_length * bytes_per_pixel;
            const size_t destination =
                whole_copy.buffer_offset +
                (size_t{copy.image_offset.y + row} * whole_copy.buffer_row_length +
                 copy.image_offset.x) * bytes_per_pixel;
            const size_t bytes = size_t{copy.image_extent.width} * bytes_per_pixel;
            REQUIRE(source + bytes <= output.size());
            REQUIRE(destination + bytes <= actual.size());
            CHECK(std::equal(output.begin() + source, output.begin() + source + bytes,
                             whole.begin() + destination));
            std::copy_n(output.begin() + source, bytes, actual.begin() + destination);
            for (size_t byte = destination; byte < destination + bytes; ++byte) {
                ++actual_coverage[byte];
            }
        }
    }
    CHECK(consumed_bytes == output_size);
    CHECK(actual_coverage == expected_coverage);
    CHECK(actual == expected);
}
