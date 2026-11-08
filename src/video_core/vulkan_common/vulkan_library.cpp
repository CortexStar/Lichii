// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "video_core/vulkan_common/vulkan_overlay_filter.h"
#endif

#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#ifdef _WIN32
#include "common/scope_exit.h"
#endif
#include "video_core/vulkan_common/vulkan_library.h"

namespace Vulkan {

#ifdef _WIN32
namespace {

void DisableOverwolfLayers() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::set<std::string> names;
        std::map<std::string, std::string> environment;
        for (const auto hive : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
            for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
                for (const auto subkey : {L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers",
                                          L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers"}) {
                    HKEY key{};
                    if (RegOpenKeyExW(hive, subkey, 0, KEY_QUERY_VALUE | view, &key) !=
                        ERROR_SUCCESS) {
                        continue;
                    }
                    SCOPE_EXIT {
                        RegCloseKey(key);
                    };
                    for (DWORD index = 0;; ++index) {
                        std::wstring path(32768, L'\0');
                        DWORD length = static_cast<DWORD>(path.size());
                        DWORD type{}, enabled{}, size = sizeof(enabled);
                        const auto result =
                            RegEnumValueW(key, index, path.data(), &length, nullptr, &type,
                                          reinterpret_cast<BYTE*>(&enabled), &size);
                        if (result == ERROR_NO_MORE_ITEMS) {
                            break;
                        }
                        if (result != ERROR_SUCCESS || type != REG_DWORD ||
                            size != sizeof(enabled) || enabled != 0) {
                            continue;
                        }
                        path.resize(length);
                        try {
                            const std::filesystem::path manifest_path{path};
                            std::error_code error;
                            const auto bytes = std::filesystem::file_size(manifest_path, error);
                            if (error || bytes > 1024 * 1024) {
                                continue;
                            }
                            std::ifstream file{manifest_path};
                            if (!file) {
                                continue;
                            }
                            const auto manifest = nlohmann::json::parse(file, nullptr, false);
                            const auto utf8 = manifest_path.u8string();
                            Detail::CollectOverwolfLayerControls(
                                manifest, std::string{utf8.begin(), utf8.end()}, names,
                                environment);
                        } catch (const std::exception& exception) {
                            LOG_WARNING(Render_Vulkan,
                                        "Skipping unreadable Vulkan layer manifest: {}",
                                        exception.what());
                        }
                    }
                }
            }
        }
        if (!names.empty()) {
            std::string disabled;
            if (const auto* current = std::getenv("VK_LOADER_LAYERS_DISABLE")) {
                disabled = current;
            }
            for (const auto& name : names) {
                if (!disabled.empty()) {
                    disabled += ',';
                }
                disabled += name;
            }
            environment.insert_or_assign("VK_LOADER_LAYERS_DISABLE", std::move(disabled));
        }
        for (const auto& [name, value] : environment) {
            if (_putenv_s(name.c_str(), value.c_str()) != 0) {
                LOG_WARNING(Render_Vulkan, "Could not set Overwolf layer disable control {}", name);
            }
        }
        if (!names.empty()) {
            LOG_INFO(Render_Vulkan,
                     "Disabled {} installed Overwolf Vulkan layer(s) for this process",
                     names.size());
        }
    });
}

} // namespace
#endif

#ifdef __APPLE__
namespace {
// MoltenVK reads these when the first instance is created. A value already in the environment
// wins, and EDEN_NX2_MVK_TUNING=0 leaves MoltenVK's own defaults.
void ApplyMoltenVKDefaults() {
    if (const char* tuning = std::getenv("EDEN_NX2_MVK_TUNING"); tuning && tuning[0] == '0') {
        LOG_INFO(Render_Vulkan, "MoltenVK tuning off (EDEN_NX2_MVK_TUNING=0)");
        return;
    }
    static constexpr std::pair<const char*, const char*> defaults[] = {
        // Compile Metal pipelines on every core rather than a few (macOS 13.3+).
        {"MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1"},
        // Encode and commit submissions on MoltenVK's own queue, so the scheduler's submit
        // thread no longer waits for Metal encoding.
        {"MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0"},
    };
    for (const auto& [name, value] : defaults) {
        setenv(name, value, 0);
        LOG_INFO(Render_Vulkan, "MoltenVK {}={}", name, std::getenv(name));
    }
}
} // Anonymous namespace
#endif

std::shared_ptr<Common::DynamicLibrary> OpenLibrary(
    [[maybe_unused]] Core::Frontend::GraphicsContext* context) {
#ifdef _WIN32
    DisableOverwolfLayers();
#endif
    LOG_DEBUG(Render_Vulkan, "Looking for a Vulkan library");
#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)
    // Android manages its Vulkan driver from the frontend.
    return context->GetDriverLibrary();
#else
    auto library = std::make_shared<Common::DynamicLibrary>();
#ifdef __APPLE__
    static const bool mvk_defaults_applied = (ApplyMoltenVKDefaults(), true);
    (void)mvk_defaults_applied;
    const auto libvulkan_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libvulkan.1.dylib";
    const auto libmoltenvk_filename =
        Common::FS::GetBundleDirectory() / "Contents/Frameworks/libMoltenVK.dylib";
    const char* library_paths[] = {std::getenv("LIBVULKAN_PATH"), libvulkan_filename.c_str(),
                                   libmoltenvk_filename.c_str()};
    // Check if a path to a specific Vulkan library has been specified.
    for (const auto& library_path : library_paths) {
        if (library_path && library->Open(library_path)) {
            break;
        }
    }
#else
    std::string filename = Common::DynamicLibrary::GetVersionedFilename("vulkan", 1);
    LOG_DEBUG(Render_Vulkan, "Trying Vulkan library: {}", filename);
    if (!library->Open(filename.c_str())) {
        // Android devices may not have libvulkan.so.1, only libvulkan.so.
        filename = Common::DynamicLibrary::GetVersionedFilename("vulkan");
        LOG_DEBUG(Render_Vulkan, "Trying Vulkan library (second attempt): {}", filename);
        void(library->Open(filename.c_str()));
    }
#endif
    return library;
#endif
}

} // namespace Vulkan
