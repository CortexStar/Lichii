// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "system_monitor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "common/polyfill_thread.h"
#include "common/thread.h"

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
// clang-format on
#elif defined(__linux__)
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

namespace {

constexpr auto SAMPLE_INTERVAL = std::chrono::milliseconds{500};

#ifdef _WIN32

u64 ToU64(const FILETIME& ft) {
    return (u64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// GPU counter instances look like "pid_1234_luid_0x00000000_0x0000D1B5_phys_0_eng_0_engtype_3D".
// The luid identifies the adapter, so per-process values can be summed per GPU.
std::string AdapterKey(const wchar_t* instance) {
    std::string name;
    for (const wchar_t* c = instance; *c; ++c) {
        name.push_back(char(*c));
    }
    name = Lower(name);
    const auto begin = name.find("luid_");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = name.find("_phys", begin);
    return name.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::string AdapterKey(const LUID& luid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "luid_0x%08lx_0x%08lx", static_cast<unsigned long>(luid.HighPart),
                  static_cast<unsigned long>(luid.LowPart));
    return buf;
}

class Sampler {
public:
    Sampler() {
        if (PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0,
                                  &gpu_counter);
            PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0,
                                  &vram_counter);
            PdhCollectQueryData(query);
        }
        FindMainAdapter();
    }

    ~Sampler() {
        if (query) {
            PdhCloseQuery(query);
        }
    }

    void Sample(SystemMonitor::Snapshot& out) {
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const u64 i = ToU64(idle), k = ToU64(kernel), u = ToU64(user);
            const u64 total = (k - last_kernel) + (u - last_user);
            if (last_kernel != 0 && total > 0) {
                const u64 idle_delta = i - last_idle;
                out.cpu_percent = 100.0 * double(total - std::min(idle_delta, total)) / double(total);
            }
            last_idle = i;
            last_kernel = k;
            last_user = u;
        }

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (GlobalMemoryStatusEx(&mem)) {
            out.ram = SystemMonitor::Usage{mem.ullTotalPhys - mem.ullAvailPhys, mem.ullTotalPhys};
        }

        if (!query || PdhCollectQueryData(query) != ERROR_SUCCESS) {
            return;
        }

        if (gpu_counter) {
            std::map<std::string, double> per_adapter;
            ForEachItem(gpu_counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                        [&](const PDH_FMT_COUNTERVALUE_ITEM_W& item) {
                            per_adapter[AdapterKey(item.szName)] += item.FmtValue.doubleValue;
                        });
            if (!per_adapter.empty()) {
                double busiest = 0.0;
                for (const auto& [key, value] : per_adapter) {
                    busiest = std::max(busiest, value);
                }
                out.gpu_percent = std::min(busiest, 100.0);
            }
        }

        if (vram_counter && main_adapter_vram > 0) {
            ForEachItem(vram_counter, PDH_FMT_LARGE, [&](const PDH_FMT_COUNTERVALUE_ITEM_W& item) {
                if (AdapterKey(item.szName) == main_adapter_key) {
                    out.vram = SystemMonitor::Usage{u64(item.FmtValue.largeValue), main_adapter_vram};
                }
            });
        }
    }

private:
    // The adapter with the most dedicated memory is the one games render on
    void FindMainAdapter() {
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            return;
        }
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                desc.DedicatedVideoMemory > main_adapter_vram) {
                main_adapter_vram = desc.DedicatedVideoMemory;
                main_adapter_key = AdapterKey(desc.AdapterLuid);
            }
            adapter->Release();
        }
        factory->Release();
    }

    template <typename F>
    void ForEachItem(PDH_HCOUNTER counter, DWORD format, F&& func) {
        DWORD size = 0;
        DWORD count = 0;
        if (PdhGetFormattedCounterArrayW(counter, format, &size, &count, nullptr) != PDH_MORE_DATA) {
            return;
        }
        buffer.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(counter, format, &size, &count, items) != ERROR_SUCCESS) {
            return;
        }
        for (DWORD i = 0; i < count; ++i) {
            const auto status = items[i].FmtValue.CStatus;
            if (status == PDH_CSTATUS_VALID_DATA || status == PDH_CSTATUS_NEW_DATA) {
                func(items[i]);
            }
        }
    }

    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpu_counter = nullptr;
    PDH_HCOUNTER vram_counter = nullptr;
    std::vector<std::byte> buffer;

    std::string main_adapter_key;
    u64 main_adapter_vram = 0;

    u64 last_idle = 0;
    u64 last_kernel = 0;
    u64 last_user = 0;
};

#elif defined(__linux__)

template <typename T>
std::optional<T> ReadValue(const std::filesystem::path& path) {
    std::ifstream file(path);
    T value{};
    if (file >> value) {
        return value;
    }
    return std::nullopt;
}

class Sampler {
public:
    Sampler() {
        // Only amdgpu exposes load and VRAM through sysfs
        std::error_code ec;
        u64 best_total = 0;
        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
            const auto device = entry.path() / "device";
            const auto total = ReadValue<u64>(device / "mem_info_vram_total");
            if (total && *total > best_total && std::filesystem::exists(device / "gpu_busy_percent")) {
                best_total = *total;
                gpu_device = device;
            }
        }
    }

    void Sample(SystemMonitor::Snapshot& out) {
        if (std::ifstream stat("/proc/stat"); stat) {
            std::string cpu;
            u64 user, nice, system, idle, iowait, irq, softirq, steal;
            if (stat >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
                const u64 idle_all = idle + iowait;
                const u64 total = user + nice + system + idle_all + irq + softirq + steal;
                if (last_total != 0 && total > last_total) {
                    const u64 delta = total - last_total;
                    const u64 idle_delta = std::min(idle_all - last_idle, delta);
                    out.cpu_percent = 100.0 * double(delta - idle_delta) / double(delta);
                }
                last_total = total;
                last_idle = idle_all;
            }
        }

        if (std::ifstream meminfo("/proc/meminfo"); meminfo) {
            std::string key;
            u64 value;
            std::string unit;
            u64 total = 0, available = 0;
            while (meminfo >> key >> value >> unit) {
                if (key == "MemTotal:") {
                    total = value * 1024;
                } else if (key == "MemAvailable:") {
                    available = value * 1024;
                }
            }
            if (total) {
                out.ram = SystemMonitor::Usage{total - std::min(available, total), total};
            }
        }

        if (!gpu_device.empty()) {
            if (const auto busy = ReadValue<double>(gpu_device / "gpu_busy_percent")) {
                out.gpu_percent = *busy;
            }
            const auto used = ReadValue<u64>(gpu_device / "mem_info_vram_used");
            const auto total = ReadValue<u64>(gpu_device / "mem_info_vram_total");
            if (used && total) {
                out.vram = SystemMonitor::Usage{*used, *total};
            }
        }
    }

private:
    std::filesystem::path gpu_device;
    u64 last_total = 0;
    u64 last_idle = 0;
};

#else

class Sampler {
public:
    void Sample(SystemMonitor::Snapshot&) {}
};

#endif

} // namespace

SystemMonitor::SystemMonitor() = default;

SystemMonitor::~SystemMonitor() = default;

void SystemMonitor::Start() {
    if (!thread.joinable()) {
        thread = std::jthread([this](std::stop_token token) { Run(token); });
    }
}

void SystemMonitor::Stop() {
    thread = {};
    std::scoped_lock lock{mutex};
    snapshot = {};
}

SystemMonitor::Snapshot SystemMonitor::Get() const {
    std::scoped_lock lock{mutex};
    return snapshot;
}

void SystemMonitor::Run(std::stop_token token) {
    Common::SetCurrentThreadName("SystemMonitor");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);

    // Counter setup (PDH, DXGI) is slow enough that it stays off the UI thread
    Sampler sampler;
    do {
        Snapshot next;
        sampler.Sample(next);
        std::scoped_lock lock{mutex};
        snapshot = next;
    } while (Common::StoppableTimedWait(token, SAMPLE_INTERVAL));
}
