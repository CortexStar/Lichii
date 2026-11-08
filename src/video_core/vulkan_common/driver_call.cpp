// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/vulkan_common/driver_call.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"

namespace Vulkan::DriverCall {
namespace {

thread_local const Record* current_record{};
thread_local const Context* current_context{};
thread_local const Record* fault_record{};
thread_local std::string fault_folder;

/// Appends to a fixed buffer, dropping what does not fit; no allocation.
class Writer {
public:
    Writer(char* buffer_, size_t size_) : buffer{buffer_}, size{size_} {
        if (size != 0) {
            buffer[0] = '\0';
        }
    }

    template <typename... Args>
    void Append(fmt::format_string<Args...> format, Args&&... args) {
        if (length + 1 >= size) {
            return;
        }
        const auto result = fmt::format_to_n(buffer + length, size - 1 - length, format,
                                             std::forward<Args>(args)...);
        length += (std::min)(result.size, size - 1 - length);
        buffer[length] = '\0';
    }

    /// The names of the set bits of `flags`, '|'-separated, through `name` (one bit to a name).
    template <typename Bits, typename NameFn>
    void Flags(u64 flags, NameFn&& name) {
        if (flags == 0) {
            Append("0");
            return;
        }
        bool first = true;
        while (flags != 0) {
            const u64 bit = flags & (~flags + 1);
            flags &= flags - 1;
            Append("{}{}", first ? "" : "|", name(static_cast<Bits>(bit)));
            first = false;
        }
    }

    [[nodiscard]] size_t Length() const {
        return length;
    }

private:
    char* buffer;
    size_t size;
    size_t length{};
};

/// "VK_SHADER_STAGE_FRAGMENT_BIT" -> "FRAGMENT", and likewise for the other flag names.
std::string_view Trim(std::string_view name, std::string_view prefix) {
    if (name.starts_with(prefix)) {
        name.remove_prefix(prefix.size());
    }
    if (name.ends_with("_BIT")) {
        name.remove_suffix(4);
    }
    return name;
}

template <typename T>
const T* FindNext(const void* next, VkStructureType type) {
    for (auto* header = static_cast<const VkBaseInStructure*>(next); header != nullptr;
         header = header->pNext) {
        if (header->sType == type) {
            return reinterpret_cast<const T*>(header);
        }
    }
    return nullptr;
}

void DescribeStage(Writer& out, const VkPipelineShaderStageCreateInfo& stage) {
    out.Append(" {}", Trim(string_VkShaderStageFlagBits(stage.stage), "VK_SHADER_STAGE_"));
    if (const auto* inline_code = FindNext<VkShaderModuleCreateInfo>(
            stage.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)) {
        out.Append(":spirv={:016x}",
                   SpirvHash(SpirvWords(inline_code->pCode, inline_code->codeSize)));
    } else {
        out.Append(":module={:#x}", reinterpret_cast<uintptr_t>(stage.module));
    }
    if (stage.pSpecializationInfo != nullptr && stage.pSpecializationInfo->mapEntryCount != 0) {
        out.Append("(spec {})", stage.pSpecializationInfo->mapEntryCount);
    }
}

void DescribeGraphics(Writer& out, const VkGraphicsPipelineCreateInfo& ci) {
    out.Append(" flags={:#x}", static_cast<u64>(ci.flags));
    if (const auto* library = FindNext<VkGraphicsPipelineLibraryCreateInfoEXT>(
            ci.pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)) {
        out.Append(" library=");
        out.Flags<VkGraphicsPipelineLibraryFlagBitsEXT>(library->flags, [](auto bit) {
            return Trim(string_VkGraphicsPipelineLibraryFlagBitsEXT(bit),
                        "VK_GRAPHICS_PIPELINE_LIBRARY_");
        });
    }
    if (const auto* link = FindNext<VkPipelineLibraryCreateInfoKHR>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)) {
        out.Append(" links={}", link->libraryCount);
    }
    out.Append(" stages={}", ci.stageCount);
    for (u32 i = 0; i < ci.stageCount && ci.pStages != nullptr; ++i) {
        DescribeStage(out, ci.pStages[i]);
    }
    if (const auto* rendering = FindNext<VkPipelineRenderingCreateInfo>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)) {
        out.Append(" colors=");
        for (u32 i = 0; i < rendering->colorAttachmentCount; ++i) {
            out.Append("{}{}", i == 0 ? "" : ",",
                       Trim(string_VkFormat(rendering->pColorAttachmentFormats[i]), "VK_FORMAT_"));
        }
        out.Append(" depth={}",
                   Trim(string_VkFormat(rendering->depthAttachmentFormat), "VK_FORMAT_"));
    }
    out.Append(" layout={:#x}", reinterpret_cast<uintptr_t>(ci.layout));
}

void DescribeInfo(Writer& out, const Record& record) {
    if (record.info == nullptr) {
        return;
    }
    switch (record.kind) {
    case Kind::Other:
        break;
    case Kind::GraphicsPipeline:
        DescribeGraphics(out, *static_cast<const VkGraphicsPipelineCreateInfo*>(record.info));
        break;
    case Kind::ComputePipeline: {
        const auto& ci = *static_cast<const VkComputePipelineCreateInfo*>(record.info);
        out.Append(" flags={:#x}", static_cast<u64>(ci.flags));
        DescribeStage(out, ci.stage);
        out.Append(" layout={:#x}", reinterpret_cast<uintptr_t>(ci.layout));
        break;
    }
    case Kind::ShaderModule: {
        const auto& ci = *static_cast<const VkShaderModuleCreateInfo*>(record.info);
        out.Append(" spirv={:016x} words={}", SpirvHash(SpirvWords(ci.pCode, ci.codeSize)),
                   SpirvWords(ci.pCode, ci.codeSize).size());
        break;
    }
    case Kind::Shaders: {
        const auto* infos = static_cast<const VkShaderCreateInfoEXT*>(record.info);
        for (u32 i = 0; i < record.count; ++i) {
            const VkShaderCreateInfoEXT& ci = infos[i];
            out.Append(" {}", Trim(string_VkShaderStageFlagBits(ci.stage), "VK_SHADER_STAGE_"));
            if (ci.codeType == VK_SHADER_CODE_TYPE_SPIRV_EXT) {
                out.Append(":spirv={:016x}", SpirvHash(SpirvWords(ci.pCode, ci.codeSize)));
            } else {
                out.Append(":binary");
            }
        }
        break;
    }
    case Kind::Buffer: {
        const auto& ci = *static_cast<const VkBufferCreateInfo*>(record.info);
        out.Append(" size={:#x} usage=", ci.size);
        u64 usage = ci.usage;
        if (const auto* usage2 = FindNext<VkBufferUsageFlags2CreateInfo>(
                ci.pNext, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO)) {
            usage = usage2->usage;
        }
        out.Flags<VkBufferUsageFlagBits2>(usage, [](auto bit) {
            return Trim(string_VkBufferUsageFlagBits2(bit), "VK_BUFFER_USAGE_2_");
        });
        if (ci.flags != 0) {
            out.Append(" flags={:#x}", static_cast<u64>(ci.flags));
        }
        break;
    }
    case Kind::Image: {
        const auto& ci = *static_cast<const VkImageCreateInfo*>(record.info);
        out.Append(" {} {}x{}x{} levels={} layers={} samples={} {} usage=",
                   Trim(string_VkImageType(ci.imageType), "VK_IMAGE_TYPE_"), ci.extent.width,
                   ci.extent.height, ci.extent.depth, ci.mipLevels, ci.arrayLayers,
                   static_cast<u32>(ci.samples), Trim(string_VkFormat(ci.format), "VK_FORMAT_"));
        out.Flags<VkImageUsageFlagBits>(ci.usage, [](auto bit) {
            return Trim(string_VkImageUsageFlagBits(bit), "VK_IMAGE_USAGE_");
        });
        out.Append(" tiling={}", Trim(string_VkImageTiling(ci.tiling), "VK_IMAGE_TILING_"));
        if (ci.flags != 0) {
            out.Append(" flags={:#x}", static_cast<u64>(ci.flags));
        }
        break;
    }
    }
}

} // Anonymous namespace

Scope::Scope(const char* call, Kind kind, const void* info, u32 count) noexcept
    : record{call, kind, count, info, current_record} {
    current_record = &record;
}

Scope::~Scope() {
    current_record = record.outer;
    if (fault_record == &record) {
        LOG_WARNING(Render_Vulkan, "{} returned after the driver handled its own fault (repro: {})",
                    record.call, fault_folder);
        fault_record = nullptr;
    }
}

ContextScope::ContextScope(const char* label, u64 key, std::span<const u64> hashes) noexcept
    : context{label, key, {}, current_context} {
    std::copy_n(hashes.begin(), (std::min)(hashes.size(), context.hashes.size()),
                context.hashes.begin());
    current_context = &context;
}

ContextScope::~ContextScope() {
    current_context = context.outer;
}

const Record* Current() noexcept {
    return current_record;
}

const Context* CurrentContext() noexcept {
    return current_context;
}

size_t Describe(char* buffer, size_t size, const Record* record, const Context* context) noexcept {
    Writer out{buffer, size};
    if (record != nullptr) {
        out.Append("{}(", record->call);
        DescribeInfo(out, *record);
        out.Append(" )");
    }
    if (context != nullptr) {
        out.Append("{}for {} {:016x}", record != nullptr ? " " : "", context->label, context->key);
        for (size_t stage = 0; stage < context->hashes.size(); ++stage) {
            if (context->hashes[stage] != 0) {
                out.Append(" s{}:{:016x}", stage, context->hashes[stage]);
            }
        }
    }
    return out.Length();
}

std::string DescribeCurrent() {
    if (current_record == nullptr && current_context == nullptr) {
        return {};
    }
    std::array<char, 1024> buffer;
    const size_t length = Describe(buffer.data(), buffer.size(), current_record, current_context);
    return std::string(buffer.data(), length);
}

const char* FaultArtifact(const Record* record) noexcept {
    return record != nullptr && record == fault_record ? fault_folder.c_str() : nullptr;
}

void NoteFault(const Record* record, std::string folder) {
    fault_record = record;
    fault_folder = std::move(folder);
}

u64 SpirvHash(std::span<const u32> code) noexcept {
    if (code.data() == nullptr) {
        return 0;
    }
    return Common::CityHash64(reinterpret_cast<const char*>(code.data()), code.size_bytes());
}

std::string_view ShortFunctionName(std::string_view function) noexcept {
    // Cut the parameter list, then the return type; keep the last two scopes.
    if (const size_t paren = function.find('('); paren != std::string_view::npos) {
        function = function.substr(0, paren);
    }
    if (const size_t space = function.rfind(' '); space != std::string_view::npos) {
        function.remove_prefix(space + 1);
    }
    if (const size_t last = function.rfind("::"); last != std::string_view::npos && last != 0) {
        if (const size_t before = function.rfind("::", last - 1);
            before != std::string_view::npos) {
            function.remove_prefix(before + 2);
        }
    }
    return function;
}

} // namespace Vulkan::DriverCall
