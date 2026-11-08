// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_binary_store.h"

namespace {
using Vulkan::PipelineBinaryBlob;
using Vulkan::PipelineBinaryKey;
using Vulkan::PipelineBinaryStore;

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-pipeline-binary-store-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

PipelineBinaryKey Key(u8 value, u32 size = 32) {
    PipelineBinaryKey key;
    key.size = size;
    key.bytes.fill(value);
    return key;
}

std::vector<PipelineBinaryBlob> Blobs(u8 value, size_t count, size_t size) {
    std::vector<PipelineBinaryBlob> blobs;
    for (size_t i = 0; i < count; ++i) {
        blobs.push_back({Key(static_cast<u8>(value + i)), std::vector<u8>(size, value)});
    }
    return blobs;
}

bool Same(const std::vector<PipelineBinaryBlob>& a, const std::vector<PipelineBinaryBlob>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].key != b[i].key || a[i].data != b[i].data) {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("Pipeline binaries survive a reopen and a later record replaces an earlier one",
          "[video_core][pipeline_binary]") {
    TempDir dir;
    const auto file = dir.path / "vulkan_binaries.bin";
    {
        PipelineBinaryStore store;
        REQUIRE(store.Open(file, Key(0xAA, 20)));
        REQUIRE_FALSE(store.Find(Key(1)));
        REQUIRE(store.Store(Key(1), Blobs(10, 2, 100)));
        REQUIRE(store.Store(Key(2), Blobs(20, 1, 5000)));
        REQUIRE(store.Store(Key(1), Blobs(30, 3, 7)));
        const auto found = store.Find(Key(1));
        REQUIRE(found);
        REQUIRE(Same(*found, Blobs(30, 3, 7)));
        REQUIRE(store.Size() == 2);
    }
    PipelineBinaryStore store;
    REQUIRE(store.Open(file, Key(0xAA, 20)));
    REQUIRE(store.Size() == 2);
    const auto first = store.Find(Key(1));
    const auto second = store.Find(Key(2));
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(Same(*first, Blobs(30, 3, 7)));
    REQUIRE(Same(*second, Blobs(20, 1, 5000)));
}

TEST_CASE("Pipeline binaries of another driver are dropped", "[video_core][pipeline_binary]") {
    TempDir dir;
    const auto file = dir.path / "vulkan_binaries.bin";
    {
        PipelineBinaryStore store;
        REQUIRE(store.Open(file, Key(0xAA)));
        REQUIRE(store.Store(Key(1), Blobs(10, 1, 64)));
    }
    PipelineBinaryStore store;
    REQUIRE(store.Open(file, Key(0xAB)));
    REQUIRE(store.Size() == 0);
    REQUIRE_FALSE(store.Find(Key(1)));
    REQUIRE(store.Store(Key(3), Blobs(40, 1, 8)));
    REQUIRE(store.Find(Key(3)));
}

TEST_CASE("A pipeline binary record cut off at the end of the file is dropped",
          "[video_core][pipeline_binary]") {
    TempDir dir;
    const auto file = dir.path / "vulkan_binaries.bin";
    uintmax_t complete_size{};
    {
        PipelineBinaryStore store;
        REQUIRE(store.Open(file, Key(0xAA)));
        REQUIRE(store.Store(Key(1), Blobs(10, 1, 64)));
        complete_size = std::filesystem::file_size(file);
        REQUIRE(store.Store(Key(2), Blobs(20, 2, 4096)));
    }
    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 100);
    {
        PipelineBinaryStore store;
        REQUIRE(store.Open(file, Key(0xAA)));
        REQUIRE(store.Size() == 1);
        REQUIRE(store.Find(Key(1)));
        REQUIRE_FALSE(store.Find(Key(2)));
        // The damaged tail is gone, so a new record follows the last complete one.
        REQUIRE(std::filesystem::file_size(file) == complete_size);
        REQUIRE(store.Store(Key(2), Blobs(50, 1, 16)));
    }
    PipelineBinaryStore store;
    REQUIRE(store.Open(file, Key(0xAA)));
    REQUIRE(store.Size() == 2);
    const auto found = store.Find(Key(2));
    REQUIRE(found);
    REQUIRE(Same(*found, Blobs(50, 1, 16)));
}

TEST_CASE("Pipeline binary records with no or too many binaries are refused",
          "[video_core][pipeline_binary]") {
    TempDir dir;
    PipelineBinaryStore store;
    REQUIRE(store.Open(dir.path / "vulkan_binaries.bin", Key(0xAA)));
    REQUIRE_FALSE(store.Store(Key(1), {}));
    REQUIRE_FALSE(
        store.Store(Key(1), Blobs(1, PipelineBinaryStore::MaxBinariesPerPipeline + 1, 4)));
    REQUIRE(store.Size() == 0);
}
