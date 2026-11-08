// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "common/fs/fs_util.h"
#include "common/fs/mapped_file.h"

namespace Common::FS {

std::unique_ptr<MappedFile> MappedFile::Open(std::string_view utf8_path) {
    const std::filesystem::path path{ToU8String(utf8_path)};
    std::unique_ptr<MappedFile> file{new MappedFile};
#ifdef _WIN32
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0) {
        CloseHandle(handle);
        return nullptr;
    }
    // The mapping keeps the file open; the handle is not needed past this point.
    const HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(handle);
    if (mapping == nullptr) {
        return nullptr;
    }
    void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        CloseHandle(mapping);
        return nullptr;
    }
    file->mapping = mapping;
    file->data = static_cast<const u8*>(view);
    file->size = static_cast<std::size_t>(size.QuadPart);
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return nullptr;
    }
    void* const view =
        ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (view == MAP_FAILED) {
        return nullptr;
    }
    file->data = static_cast<const u8*>(view);
    file->size = static_cast<std::size_t>(st.st_size);
#endif
    return file;
}

MappedFile::~MappedFile() {
#ifdef _WIN32
    UnmapViewOfFile(data);
    CloseHandle(mapping);
#else
    ::munmap(const_cast<u8*>(data), size);
#endif
}

std::size_t MappedFile::Read(u8* out, std::size_t length, std::size_t offset) const {
    if (offset >= size) {
        return 0;
    }
    const std::size_t count = (std::min)(length, size - offset);
    std::memcpy(out, data + offset, count);
    return count;
}

} // namespace Common::FS
