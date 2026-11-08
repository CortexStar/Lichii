// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdio>
#include <span>
#include <string>

#include "common/common_types.h"
#include "video_core/vulkan_common/driver_call.h"

/// EDEN_NX2_CRASH_DUMP=<dir>: when the driver crashes inside a create call (DriverCall::Scope),
/// write a repro folder <dir>/driver-crash-<pid>-<n>/ with the call's SPIR-V (every stage, also
/// those of linked pipeline libraries), its pipeline state and a summary, and log one line naming
/// it. A vectored exception handler catches the fault first, before any handler the driver or
/// another library installs. Off: one predicted branch per shader module and pipeline.
namespace Vulkan::DriverCrash {

/// The dump folder (EDEN_NX2_CRASH_DUMP), or empty when crash dumps are off. The first call
/// installs the exception handler.
[[nodiscard]] const std::string& Directory();

[[nodiscard]] inline bool Enabled() {
    return !Directory().empty();
}

/// Keep the SPIR-V of live shader modules and pipeline libraries, so a crash in a later pipeline
/// build or link can still dump it (Lichii frees modules once their pipeline exists).
void RegisterModule(VkShaderModule module, std::span<const u32> code);
void ForgetModule(VkShaderModule module);
/// Records the SPIR-V a pipeline library (or a pipeline linked from libraries) was built from.
void RegisterPipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo& ci);
void ForgetPipeline(VkPipeline pipeline);

/// One SPIR-V program of a call: the stage it is for and its words.
struct Program {
    VkShaderStageFlagBits stage;
    std::span<const u32> code;
};

/// Writes the repro folder for `record` (within `context`) into `directory`: summary.txt, one
/// <n>-<stage>-<spirv hash>.spv per program, state.txt. `reason` heads the summary. Returns the
/// folder, or empty if it could not be created.
std::string WriteArtifact(const std::string& directory, const DriverCall::Record& record,
                          const DriverCall::Context* context, const char* reason);

/// The full create state of `record` as text (pipeline state, stages, specialization constants,
/// pNext chain), as state.txt holds it.
void WriteState(std::FILE* file, const DriverCall::Record& record);

} // namespace Vulkan::DriverCrash
