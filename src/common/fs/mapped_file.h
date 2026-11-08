// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

#include "common/common_types.h"

namespace Common::FS {

/// A whole file mapped read-only into the address space. Reads are copies out of the OS page
/// cache: no seek, no read call and no lock, so threads read it at the same time.
class MappedFile {
public:
    /// Maps the file at the UTF-8 path, or returns nullptr (missing, empty, or the OS refused).
    static std::unique_ptr<MappedFile> Open(std::string_view utf8_path);

    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::span<const u8> Data() const {
        return {data, size};
    }

    /// Copies up to length bytes from offset; returns the bytes copied (0 past the end).
    std::size_t Read(u8* out, std::size_t length, std::size_t offset) const;

private:
    MappedFile() = default;

    const u8* data{};
    std::size_t size{};
#ifdef _WIN32
    void* mapping{};
#endif
};

} // namespace Common::FS
