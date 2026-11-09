// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Linux: NVIDIA DLSS through NVIDIA NGX directly (Streamline has no Linux build). Same
// interface as streamline.cpp. NGX's core comes with the driver (libnvidia-ngx.so); the DLSS
// model (libnvidia-ngx-dlss.so.*) is searched next to the emulator and in the streamline folder.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>


#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/streamline.h"

#include <nvsdk_ngx_vk.h>

namespace Vulkan::Streamline {
namespace {

// Own project id for NVSDK_NGX_VULKAN_Init_with_ProjectID (no NVIDIA application id).
constexpr const char* PROJECT_ID = "6f1f3a52-8f4c-4b8e-9a51-1c0de5a7ee11";

std::filesystem::path ExeDir() {
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
}

struct State {
    bool started = false;
    bool attached = false;
    bool available = false;
    VkDevice device = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* caps = nullptr;
};

State& GetState() {
    static State state;
    return state;
}

std::vector<std::filesystem::path> SearchDirs() {
    return {ExeDir(), UserFolder()};
}

std::filesystem::path FindModel(std::string& version) {
    std::filesystem::path best;
    for (const std::filesystem::path& dir : SearchDirs()) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            const std::string name = entry.path().filename().string();
            constexpr std::string_view prefix = "libnvidia-ngx-dlss.so.";
            if (name.starts_with(prefix) && name > best.filename().string()) {
                best = entry.path();
                version = name.substr(prefix.size());
            }
        }
    }
    return best;
}

void NVSDK_CONV LogFromNgx(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    std::string text{message};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    LOG_DEBUG(Render_Vulkan, "NGX: {}", text);
}

std::vector<std::string> RequiredExtensions(bool device) {
    unsigned int instance_count = 0, device_count = 0;
    const char** instance_names = nullptr;
    const char** device_names = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&instance_count, &instance_names,
                                                             &device_count, &device_names))) {
        return {};
    }
    const unsigned int count = device ? device_count : instance_count;
    const char** names = device ? device_names : instance_names;
    return std::vector<std::string>(names, names + count);
}

} // Anonymous namespace

std::filesystem::path UserFolder() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::StreamlineDir);
}

bool IsStreamlineModule(const std::filesystem::path&) {
    return false;
}

bool IsSignedByNvidia(const std::filesystem::path&) {
    return false;
}

DlssFiles FindDlssFiles() {
    DlssFiles files;
    files.folder = ExeDir();
    files.model = FindModel(files.model_version);
    return files;
}

bool Start() {
    State& state = GetState();
    if (state.started) {
        return true;
    }
    if (!Settings::values.nvidia_dlss.GetValue()) {
        return false;
    }
    std::string version;
    if (FindModel(version).empty()) {
        LOG_WARNING(Render_Vulkan,
                    "NVIDIA DLSS is on, but libnvidia-ngx-dlss.so.* is not in {} or {}; using "
                    "FSR 3.1",
                    Common::FS::PathToUTF8String(ExeDir()),
                    Common::FS::PathToUTF8String(UserFolder()));
        return false;
    }
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS through NGX, model {}", version);
    state.started = true;
    return true;
}

bool WantsDevice() {
    const State& state = GetState();
    return state.started && !state.attached;
}

std::vector<std::string> InstanceExtensions() {
    return GetState().started ? RequiredExtensions(false) : std::vector<std::string>{};
}

std::vector<std::string> DeviceExtensions() {
    return GetState().started ? RequiredExtensions(true) : std::vector<std::string>{};
}

bool AttachDevice(VkInstance instance, VkPhysicalDevice physical, VkDevice device, u32,
                  vk::DeviceDispatch& dld) {
    State& state = GetState();
    if (!state.started) {
        return false;
    }
    const std::filesystem::path data = UserFolder();
    std::error_code ec;
    std::filesystem::create_directories(data, ec);
    std::vector<std::wstring> dirs;
    for (const std::filesystem::path& dir : SearchDirs()) {
        dirs.push_back(dir.wstring());
    }
    std::vector<const wchar_t*> dir_ptrs;
    for (const std::wstring& dir : dirs) {
        dir_ptrs.push_back(dir.c_str());
    }
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = dir_ptrs.data();
    info.PathListInfo.Length = static_cast<unsigned int>(dir_ptrs.size());
    info.LoggingInfo.LoggingCallback = LogFromNgx;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = true;

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", data.wstring().c_str(), instance,
        physical, device, dld.vkGetInstanceProcAddr, dld.vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(result)) {
        LOG_WARNING(Render_Vulkan, "NGX init failed (0x{:08x}); using FSR 3.1",
                    static_cast<u32>(result));
        state.started = false;
        return false;
    }
    state.device = device;
    state.attached = true;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.caps);
    int available = 0;
    int needs_driver = 0;
    int init_result = 0;
    if (NVSDK_NGX_SUCCEED(result)) {
        NVSDK_NGX_Parameter_GetI(state.caps, NVSDK_NGX_Parameter_SuperSampling_Available,
                                 &available);
        NVSDK_NGX_Parameter_GetI(state.caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                                 &needs_driver);
        NVSDK_NGX_Parameter_GetI(state.caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult,
                                 &init_result);
    }
    state.available = available != 0;
    if (!state.available) {
        LOG_WARNING(Render_Vulkan,
                    "NGX has no DLSS on this device (caps 0x{:08x}, init 0x{:08x}, needs newer "
                    "driver {}); using FSR 3.1",
                    static_cast<u32>(result), static_cast<u32>(init_result), needs_driver);
        Stop();
        return false;
    }
    LOG_INFO(Render_Vulkan, "NVIDIA DLSS (NGX) ready");
    return true;
}

bool IsDlssAvailable() {
    return GetState().available;
}

void Stop() {
    State& state = GetState();
    if (state.caps) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.caps);
        state.caps = nullptr;
    }
    if (state.attached) {
        NVSDK_NGX_VULKAN_Shutdown1(state.device);
    }
    state = State{};
}

} // namespace Vulkan::Streamline
