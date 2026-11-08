// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AMD FSR API (externals/FidelityFX-API) for the Windows-only users of AMD's runtime. The
// headers put their own macros after #pragma (unknown to GCC and Clang) and declare deprecated
// structs, so their warnings are silenced here and nowhere else.

#ifndef _WIN32
#error "AMD's FSR runtime is Windows-only"
#endif

#include <cstdint>
#include <string>
#include <vector>

#include "video_core/vulkan_common/vulkan.h"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/vk/ffx_api_vk.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#include "upscalers/include/ffx_upscale.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Vulkan::AmdFfx {

/// The loader's five entry points (null until Load() succeeds).
struct Functions {
    PfnFfxCreateContext create_context{};
    PfnFfxDestroyContext destroy_context{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
};

[[nodiscard]] const Functions& Api();

struct Version {
    std::uint64_t id{};
    std::string name;
};

/// The versions the DLLs and the driver offer for an effect (ffxQueryDescGetVersions, null
/// context), newest first as the API reports them. Empty when the effect has none for the GPU.
[[nodiscard]] std::vector<Version> QueryVersions(std::uint64_t create_desc_type,
                                                 VkDeviceContext& device);

/// The provider a created context uses (ffxQueryGetProviderVersion), or "?" when unknown.
[[nodiscard]] std::string ProviderName(ffxContext& context);

/// Logs the runtime's warnings and errors (fpMessage of the create descriptions).
void Message(std::uint32_t type, const wchar_t* message);

/// A 2D image as FFX takes it: layout GENERAL is passed as GENERIC_READ (read inputs) or
/// UNORDERED_ACCESS (outputs), as the Vulkan backend expects for images it does not transition.
[[nodiscard]] FfxApiResource Image2D(VkImage image, VkFormat format, std::uint32_t width,
                                     std::uint32_t height, bool storage, std::uint32_t state);

} // namespace Vulkan::AmdFfx
