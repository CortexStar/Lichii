// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/mapped_file.h"

namespace {

std::filesystem::path TempFile(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("eden-mapped-file-" + name);
}

} // namespace

TEST_CASE("MappedFile reads ranges of the file", "[common][fs]") {
    const auto path = TempFile("ranges");
    {
        std::ofstream out(path, std::ios::binary);
        for (int i = 0; i < 10000; ++i) {
            out.put(static_cast<char>(i * 7));
        }
    }
    const auto file = Common::FS::MappedFile::Open(path.string());
    REQUIRE(file != nullptr);
    REQUIRE(file->Data().size() == 10000);

    std::array<u8, 16> buffer{};
    REQUIRE(file->Read(buffer.data(), buffer.size(), 5000) == buffer.size());
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        REQUIRE(buffer[i] == static_cast<u8>((5000 + i) * 7));
    }
    // A read across the end is cut short; one past the end reads nothing.
    REQUIRE(file->Read(buffer.data(), buffer.size(), 9996) == 4);
    REQUIRE(buffer[3] == static_cast<u8>(9999 * 7));
    REQUIRE(file->Read(buffer.data(), buffer.size(), 10000) == 0);
    REQUIRE(file->Read(buffer.data(), buffer.size(), 1u << 30) == 0);

    std::filesystem::remove(path);
}

TEST_CASE("MappedFile refuses missing and empty files", "[common][fs]") {
    const auto missing = TempFile("missing");
    std::filesystem::remove(missing);
    REQUIRE(Common::FS::MappedFile::Open(missing.string()) == nullptr);

    const auto empty = TempFile("empty");
    {
        std::ofstream out(empty, std::ios::binary);
    }
    REQUIRE(Common::FS::MappedFile::Open(empty.string()) == nullptr);
    std::filesystem::remove(empty);
}
