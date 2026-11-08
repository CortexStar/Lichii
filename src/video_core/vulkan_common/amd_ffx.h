// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// AMD's FSR runtime (amd_fidelityfx_loader_vk.dll with amd_fidelityfx_upscaler_vk.dll and
// amd_fidelityfx_framegeneration_vk.dll), for AMD FSR 4 upscaling and AMD FSR frame generation.
// The DLLs are AMD's signed, closed binaries: Lichii does not ship them and loads them from a
// folder the user chooses. Only the API headers (MIT, externals/FidelityFX-API) are compiled in.
namespace Vulkan::AmdFfx {

inline constexpr std::wstring_view LoaderName = L"amd_fidelityfx_loader_vk.dll";
inline constexpr std::wstring_view UpscalerName = L"amd_fidelityfx_upscaler_vk.dll";
inline constexpr std::wstring_view FrameGenerationName = L"amd_fidelityfx_framegeneration_vk.dll";

/// The folders searched for the loader, in order: the setting (when not empty), EDEN_FFX_DIR,
/// the user folder's "amd-fsr", then "amd-fsr" next to the executable.
[[nodiscard]] std::vector<std::filesystem::path> SearchDirs(std::string_view setting,
                                                            const char* environment,
                                                            const std::filesystem::path& user,
                                                            const std::filesystem::path& exe);

/// Loads the loader from the first folder of SearchDirs that holds it. Once per process; logs
/// the folder and which effect DLLs sit next to it. False off Windows or without the DLLs.
bool Load();

/// Load() succeeded.
[[nodiscard]] bool Loaded();

/// The folder the loader came from (empty when not loaded).
[[nodiscard]] std::filesystem::path Folder();

/// The effect DLL lies next to the loader (the loader looks there, ffx-api.md "Effect DLL
/// lookup").
[[nodiscard]] bool HasUpscaler();
[[nodiscard]] bool HasFrameGeneration();

/// Marks a call into AMD's runtime (or one of its callbacks into Lichii) for crash reports: AMD's
/// DLLs carry no symbols, so a crash handler names the calls that were running and the last ones
/// made (Breadcrumbs).
class Scope {
public:
    explicit Scope(const char* call) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    u32 slot;
};

/// The AMD runtime calls in progress and the last ones made, oldest first, for a crash handler
/// (no allocation, no locks: safe to read from one).
void WriteBreadcrumbs(char* buffer, size_t size) noexcept;

/// Whether `name` (as ffxQueryDescGetVersions reports it: "4.1.2", "FSR 4.1.2 *", ...) is a
/// version 4 or newer.
[[nodiscard]] bool IsVersion4OrNewer(std::string_view name);

/// The index of the newest version 4 or newer among `names` (by major, minor, patch), or none.
[[nodiscard]] std::optional<size_t> NewestVersion4(std::span<const std::string> names);

} // namespace Vulkan::AmdFfx
