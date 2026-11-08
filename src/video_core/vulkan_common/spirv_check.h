// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string>

#include "common/common_types.h"

/// EDEN_NX2_SPIRV_VALIDATE=<dir>: run SPIRV-Tools' validator (spirv-val) on every SPIR-V module
/// Lichii hands the driver (vkCreateShaderModule, vkCreateShadersEXT), so a shader we emitted or
/// rewrote (injections, heap lowering) that breaks a SPIR-V rule is caught before the driver
/// compiles it. A failure logs the module's hash, the guest pipeline it is for and the first
/// validator message, and writes <dir>/spirv-invalid-<hash>.{spv,spvasm,txt}. The validator is
/// SPIRV-Tools' shared library, loaded only when the switch is on (EDEN_NX2_SPIRV_TOOLS=<path>,
/// else SPIRV-Tools-shared next to the executable or on the library path): nothing is linked in.
namespace Vulkan::SpirvCheck {

/// The dump folder, or empty when off.
[[nodiscard]] const std::string& Directory();

[[nodiscard]] inline bool Enabled() {
    return !Directory().empty();
}

/// What the device enabled that changes which SPIR-V is valid.
struct Options {
    u32 api_version;
    bool uniform_buffer_standard_layout;
    bool scalar_block_layout;
    bool workgroup_scalar_block_layout;
    bool local_size_id; ///< VK_KHR_maintenance4
};

/// SPIRV-Tools' spv_target_env for a Vulkan API version (SPV_ENV_VULKAN_1_x).
[[nodiscard]] int TargetEnv(u32 api_version);

/// Loads the validator for the device's rules. Logs whether it is on.
void Configure(const Options& options);

/// Whether a validator is loaded (Configure succeeded).
[[nodiscard]] bool Ready();

/// Validates `code`. Returns an empty string when it is valid or nothing could check it, else
/// the validator's message.
[[nodiscard]] std::string Validate(std::span<const u32> code);

/// Validate(), and on a failure log it and dump the module (once per module hash).
void Check(std::span<const u32> code);

} // namespace Vulkan::SpirvCheck
