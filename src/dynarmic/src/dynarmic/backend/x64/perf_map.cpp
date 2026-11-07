// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <cstddef>
#include <string>
#include <fmt/format.h>

#include "dynarmic/backend/x64/perf_map.h"
#include "common/common_types.h"
#if defined(__linux__) && !defined(__ANDROID__)
#    include <cstdio>
#    include <cstdlib>
#    include <mutex>
#    include <sys/types.h>
#    include <unistd.h>

namespace Dynarmic::Backend::X64 {

namespace {
std::mutex mutex;
std::FILE* file = nullptr;

void OpenFile() {
    const char* perf_dir = std::getenv("PERF_BUILDID_DIR");
    if (!perf_dir) {
        file = nullptr;
        return;
    }

    const pid_t pid = getpid();
    const std::string filename = fmt::format("{:s}/perf-{:d}.map", perf_dir, pid);

    file = std::fopen(filename.c_str(), "w");
    if (!file) {
        return;
    }

    std::setvbuf(file, nullptr, _IONBF, 0);
}
}  // anonymous namespace

namespace detail {
void PerfMapRegister(const void* start, const void* end, std::string_view friendly_name) {
    if (start == end) {
        // Nothing to register
        return;
    }

    std::lock_guard guard{mutex};

    if (!file) {
        OpenFile();
        if (!file) {
            return;
        }
    }

    const std::string line = fmt::format("{:016x} {:016x} {:s}\n", reinterpret_cast<u64>(start), reinterpret_cast<u64>(end) - reinterpret_cast<u64>(start), friendly_name);
    std::fwrite(line.data(), sizeof *line.data(), line.size(), file);
}
}  // namespace detail

void PerfMapClear() {
    std::lock_guard guard{mutex};

    if (!file) {
        return;
    }

    std::fclose(file);
    file = nullptr;
    OpenFile();
}

}  // namespace Dynarmic::Backend::X64

#endif

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Dynarmic::Backend::X64 {

namespace {
std::FILE* OpenJitMap() {
    static std::FILE* const map_file = [] () -> std::FILE* {
        const char* path = std::getenv("EDEN_NX2_JIT_MAP");
        if (path == nullptr || path[0] == '\0' || (path[0] == '0' && path[1] == '\0')) {
            return nullptr;
        }
        std::FILE* f = std::fopen(path, "a");
        if (f) {
            std::setvbuf(f, nullptr, _IONBF, 0);
        }
        return f;
    }();
    return map_file;
}
std::mutex jit_map_mutex;
}  // anonymous namespace

void JitMapRegister(const void* start, std::size_t size, std::uint64_t guest_pc) {
    std::FILE* const f = OpenJitMap();
    if (!f) {
        return;
    }
    std::lock_guard guard{jit_map_mutex};
    std::fprintf(f, "%llx %llx %llx\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(start)),
                 static_cast<unsigned long long>(size), static_cast<unsigned long long>(guest_pc));
}

void JitMapCounters(const void* jit, const std::uint64_t* counters, std::size_t count) {
    std::FILE* const f = OpenJitMap();
    if (!f) {
        return;
    }
    std::lock_guard guard{jit_map_mutex};
    std::fprintf(f, "counters %llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(jit)));
    for (std::size_t i = 0; i < count; ++i) {
        std::fprintf(f, " %llu", static_cast<unsigned long long>(counters[i]));
    }
    std::fprintf(f, "\n");
}

void JitMapClear(const void* begin, const void* end) {
    std::FILE* const f = OpenJitMap();
    if (!f) {
        return;
    }
    std::lock_guard guard{jit_map_mutex};
    std::fprintf(f, "clear %llx %llx\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(begin)),
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(end)));
}

}  // namespace Dynarmic::Backend::X64
