// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <mutex>
#include <system_error>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/vulkan_common/amd_ffx.h"

#ifdef _WIN32
#include <windows.h>
#include "video_core/vulkan_common/amd_ffx_api.h"
#endif

namespace Vulkan::AmdFfx {

std::vector<std::filesystem::path> SearchDirs(std::string_view setting, const char* environment,
                                              const std::filesystem::path& user,
                                              const std::filesystem::path& exe) {
    std::vector<std::filesystem::path> dirs;
    if (!setting.empty()) {
        dirs.emplace_back(std::u8string{setting.begin(), setting.end()});
    }
    if (environment != nullptr && *environment != '\0') {
        dirs.emplace_back(environment);
    }
    if (!user.empty()) {
        dirs.push_back(user);
    }
    if (!exe.empty()) {
        dirs.push_back(exe / "amd-fsr");
    }
    return dirs;
}

namespace {

/// The first "major.minor.patch" run of digits in `name` (missing parts are 0).
std::optional<std::array<u32, 3>> ParseVersion(std::string_view name) {
    size_t i = 0;
    while (i < name.size() && (name[i] < '0' || name[i] > '9')) {
        ++i;
    }
    if (i == name.size()) {
        return std::nullopt;
    }
    std::array<u32, 3> version{};
    for (size_t part = 0; part < version.size(); ++part) {
        while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
            version[part] = version[part] * 10 + static_cast<u32>(name[i] - '0');
            ++i;
        }
        if (i + 1 >= name.size() || name[i] != '.' || name[i + 1] < '0' || name[i + 1] > '9') {
            break;
        }
        ++i;
    }
    return version;
}

} // Anonymous namespace

bool IsVersion4OrNewer(std::string_view name) {
    const auto version = ParseVersion(name);
    return version && (*version)[0] >= 4;
}

std::optional<size_t> NewestVersion4(std::span<const std::string> names) {
    std::optional<size_t> newest;
    std::array<u32, 3> newest_version{};
    for (size_t i = 0; i < names.size(); ++i) {
        const auto version = ParseVersion(names[i]);
        if (version && (*version)[0] >= 4 && (!newest || *version > newest_version)) {
            newest = i;
            newest_version = *version;
        }
    }
    return newest;
}

namespace {

struct State {
    std::once_flag once;
    bool loaded{};
    bool upscaler{};
    bool frame_generation{};
    std::filesystem::path folder;
#ifdef _WIN32
    HMODULE module{};
    Functions functions;
#endif
};

State& GetState() {
    static State state;
    return state;
}

#ifdef _WIN32
template <typename T>
bool Resolve(HMODULE module, const char* name, T& function) {
    function = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    if (function == nullptr) {
        LOG_ERROR(Render_Vulkan, "AMD FSR: {} lacks {}", Common::FS::PathToUTF8String(
                                                             std::filesystem::path{LoaderName}),
                  name);
        return false;
    }
    return true;
}

void LoadOnce(State& state) {
    const auto& setting = Settings::values.amd_fsr_dir.GetValue();
    const auto dirs =
        SearchDirs(setting, std::getenv("EDEN_FFX_DIR"),
                   Common::FS::GetEdenPath(Common::FS::EdenPath::AmdFsrDir),
                   Common::FS::GetExeDirectory());
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(dir / LoaderName, ec)) {
            continue;
        }
        // The loader finds the effect DLLs next to itself (after the executable's folder).
        HMODULE module = LoadLibraryExW((dir / LoaderName).c_str(), nullptr,
                                        LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr) {
            LOG_ERROR(Render_Vulkan, "AMD FSR: loading {} failed (error {})",
                      Common::FS::PathToUTF8String(dir / LoaderName), GetLastError());
            continue;
        }
        Functions functions;
        if (!Resolve(module, "ffxCreateContext", functions.create_context) ||
            !Resolve(module, "ffxDestroyContext", functions.destroy_context) ||
            !Resolve(module, "ffxConfigure", functions.configure) ||
            !Resolve(module, "ffxQuery", functions.query) ||
            !Resolve(module, "ffxDispatch", functions.dispatch)) {
            FreeLibrary(module);
            continue;
        }
        state.module = module;
        state.functions = functions;
        state.folder = dir;
        state.loaded = true;
        state.upscaler = std::filesystem::is_regular_file(dir / UpscalerName, ec);
        state.frame_generation = std::filesystem::is_regular_file(dir / FrameGenerationName, ec);
        LOG_INFO(Render_Vulkan, "AMD FSR: runtime from {} (upscaler DLL {}, frame generation DLL {})",
                 Common::FS::PathToUTF8String(dir), state.upscaler ? "present" : "missing",
                 state.frame_generation ? "present" : "missing");
        return;
    }
    std::string searched;
    for (const auto& dir : dirs) {
        searched += (searched.empty() ? "" : ", ") + Common::FS::PathToUTF8String(dir);
    }
    LOG_INFO(Render_Vulkan,
             "AMD FSR: {} not found in {} (AMD FSR 4 and AMD frame generation unavailable)",
             Common::FS::PathToUTF8String(std::filesystem::path{LoaderName}), searched);
}
#endif

} // Anonymous namespace

bool Load() {
    State& state = GetState();
#ifdef _WIN32
    std::call_once(state.once, [&state] { LoadOnce(state); });
#endif
    return state.loaded;
}

bool Loaded() {
    return GetState().loaded;
}

std::filesystem::path Folder() {
    return GetState().folder;
}

bool HasUpscaler() {
    return GetState().loaded && GetState().upscaler;
}

bool HasFrameGeneration() {
    return GetState().loaded && GetState().frame_generation;
}

#ifdef _WIN32
const Functions& Api() {
    return GetState().functions;
}

std::vector<Version> QueryVersions(std::uint64_t create_desc_type, VkDeviceContext& device) {
    std::vector<Version> versions;
    if (!Loaded()) {
        return versions;
    }
    std::uint64_t count = 0;
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = create_desc_type;
    query.device = &device;
    query.outputCount = &count;
    if (Api().query(nullptr, &query.header) != FFX_API_RETURN_OK || count == 0) {
        return versions;
    }
    std::vector<std::uint64_t> ids(count);
    std::vector<const char*> names(count);
    query.versionIds = ids.data();
    query.versionNames = names.data();
    if (Api().query(nullptr, &query.header) != FFX_API_RETURN_OK) {
        return versions;
    }
    // Copied at once: some names live in memory a later query overwrites (ffx-api.md).
    for (std::uint64_t i = 0; i < count; ++i) {
        versions.push_back({ids[i], names[i] != nullptr ? names[i] : "?"});
    }
    return versions;
}

std::string ProviderName(ffxContext& context) {
    ffxQueryGetProviderVersion query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (Api().query(&context, &query.header) != FFX_API_RETURN_OK || query.versionName == nullptr) {
        return "?";
    }
    return query.versionName;
}

void Message(std::uint32_t type, const wchar_t* message) {
    std::string text;
    for (; message != nullptr && *message != L'\0'; ++message) {
        text += *message < 128 ? static_cast<char>(*message) : '?';
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "AMD FSR: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "AMD FSR: {}", text);
    }
}

FfxApiResource Image2D(VkImage image, VkFormat format, std::uint32_t width, std::uint32_t height,
                       bool storage, std::uint32_t state) {
    FfxApiResource resource{};
    resource.resource = image;
    resource.state = state;
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = ffxApiGetSurfaceFormatVK(format);
    resource.description.width = width;
    resource.description.height = height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    resource.description.usage =
        (ffxApiIsDepthFormat(format) ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0U) |
        (storage ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY);
    return resource;
}
#endif

} // namespace Vulkan::AmdFfx
