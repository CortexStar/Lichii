// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

/// The driver call a thread is inside of (vkCreateGraphicsPipelines, vkCreateShaderModule,
/// vmaCreateBuffer, ...) with its create info and what Lichii built it for. A vk::Exception names
/// it, and a crash inside the driver dumps it (driver_crash.h). Two thread-local stores per call:
/// the create info is only read when something goes wrong.
namespace Vulkan::DriverCall {

enum class Kind : u8 {
    Other,
    GraphicsPipeline, ///< info: VkGraphicsPipelineCreateInfo
    ComputePipeline,  ///< info: VkComputePipelineCreateInfo
    ShaderModule,     ///< info: VkShaderModuleCreateInfo
    Shaders,          ///< info: VkShaderCreateInfoEXT[count]
    Buffer,           ///< info: VkBufferCreateInfo
    Image,            ///< info: VkImageCreateInfo
};

struct Record {
    const char* call;
    Kind kind;
    u32 count;
    const void* info;
    const Record* outer;
};

/// What Lichii is building (a guest pipeline and its stage hashes), set around the driver calls.
struct Context {
    const char* label;
    u64 key;
    std::array<u64, 6> hashes;
    const Context* outer;
};

/// Marks the calling thread as inside `call` for its lifetime. `info` must outlive the scope.
class Scope {
public:
    Scope(const char* call, Kind kind, const void* info, u32 count = 1) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Record record;
};

/// Names what the driver calls on this thread are for, e.g. ("graphics", key, stage hashes).
class ContextScope {
public:
    ContextScope(const char* label, u64 key, std::span<const u64> hashes) noexcept;
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;

private:
    Context context;
};

/// The innermost call and context of the calling thread, or null.
[[nodiscard]] const Record* Current() noexcept;
[[nodiscard]] const Context* CurrentContext() noexcept;

/// One line for `record` within `context`: the call, a create-info summary (sizes, usage, format,
/// stages and their SPIR-V hashes) and the guest pipeline. Writes no more than `size` bytes,
/// always terminated, without allocating (crash handlers call it).
size_t Describe(char* buffer, size_t size, const Record* record, const Context* context) noexcept;

/// Describe() of the calling thread's current call, or an empty string outside one.
[[nodiscard]] std::string DescribeCurrent();

/// For crash handlers: the repro folder written for `record` because the driver faulted inside
/// it, or null. NoteFault records one; if the driver then handles its own fault and the call
/// returns, the scope logs that it recovered.
[[nodiscard]] const char* FaultArtifact(const Record* record) noexcept;
void NoteFault(const Record* record, std::string folder);

/// The words of SPIR-V given as Vulkan does: a pointer and a size in bytes.
[[nodiscard]] inline std::span<const u32> SpirvWords(const void* code, size_t bytes) noexcept {
    return {static_cast<const u32*>(code), bytes / sizeof(u32)};
}

/// The hash Lichii names SPIR-V by in diagnostics (CityHash64 of the words).
[[nodiscard]] u64 SpirvHash(std::span<const u32> code) noexcept;

/// "Device::CreateBuffer" from a std::source_location function name such as
/// "Vulkan::vk::Buffer Vulkan::vk::Device::CreateBuffer(const VkBufferCreateInfo&) const".
[[nodiscard]] std::string_view ShortFunctionName(std::string_view function) noexcept;

} // namespace Vulkan::DriverCall
