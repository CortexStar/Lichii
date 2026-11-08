// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "video_core/vulkan_common/driver_crash.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"

#ifdef _WIN32
#include <windows.h>

#include "common/logging.h"
#endif

namespace Vulkan::DriverCrash {
namespace {

using Code = std::shared_ptr<const std::vector<u32>>;

struct StageCode {
    VkShaderStageFlagBits stage;
    Code code;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<VkShaderModule, Code> modules;
    std::unordered_map<VkPipeline, std::vector<StageCode>> pipelines;
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
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

/// The SPIR-V of one pipeline stage: inline (VkShaderModuleCreateInfo in pNext) or registered.
/// Call with the registry locked.
Code CopyWords(std::span<const u32> words) {
    return std::make_shared<const std::vector<u32>>(words.begin(), words.end());
}

Code StageProgram(Registry& registry, const VkPipelineShaderStageCreateInfo& stage) {
    if (const auto* inline_code = FindNext<VkShaderModuleCreateInfo>(
            stage.pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)) {
        return CopyWords(DriverCall::SpirvWords(inline_code->pCode, inline_code->codeSize));
    }
    const auto it = registry.modules.find(stage.module);
    return it != registry.modules.end() ? it->second : Code{};
}

/// Every program a graphics pipeline create uses, its linked libraries' included. Call with the
/// registry locked.
std::vector<StageCode> GraphicsPrograms(Registry& registry,
                                        const VkGraphicsPipelineCreateInfo& ci) {
    std::vector<StageCode> programs;
    for (u32 i = 0; i < ci.stageCount && ci.pStages != nullptr; ++i) {
        if (Code code = StageProgram(registry, ci.pStages[i])) {
            programs.push_back({ci.pStages[i].stage, std::move(code)});
        }
    }
    if (const auto* link = FindNext<VkPipelineLibraryCreateInfoKHR>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)) {
        for (u32 i = 0; i < link->libraryCount; ++i) {
            const auto it = registry.pipelines.find(link->pLibraries[i]);
            if (it != registry.pipelines.end()) {
                programs.insert(programs.end(), it->second.begin(), it->second.end());
            }
        }
    }
    return programs;
}

bool IsLibrary(const VkGraphicsPipelineCreateInfo& ci) {
    if ((ci.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0) {
        return true;
    }
    if (const auto* flags2 = FindNext<VkPipelineCreateFlags2CreateInfo>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)) {
        return (flags2->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) != 0;
    }
    return false;
}

/// The programs of `record`. Never blocks: a crash handler may run while another thread holds the
/// registry, and then only the inline programs are written.
std::vector<StageCode> Programs(const DriverCall::Record& record, bool& registry_busy) {
    std::vector<StageCode> programs;
    Registry& registry = GetRegistry();
    std::unique_lock lock{registry.mutex, std::try_to_lock};
    registry_busy = !lock.owns_lock();
    Registry empty;
    Registry& source = lock.owns_lock() ? registry : empty;
    switch (record.kind) {
    case DriverCall::Kind::ShaderModule: {
        const auto& ci = *static_cast<const VkShaderModuleCreateInfo*>(record.info);
        programs.push_back(
            {VK_SHADER_STAGE_ALL, CopyWords(DriverCall::SpirvWords(ci.pCode, ci.codeSize))});
        break;
    }
    case DriverCall::Kind::Shaders: {
        const auto* infos = static_cast<const VkShaderCreateInfoEXT*>(record.info);
        for (u32 i = 0; i < record.count; ++i) {
            if (infos[i].codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT) {
                continue;
            }
            programs.push_back({infos[i].stage, CopyWords(DriverCall::SpirvWords(
                                                    infos[i].pCode, infos[i].codeSize))});
        }
        break;
    }
    case DriverCall::Kind::ComputePipeline: {
        const auto& ci = *static_cast<const VkComputePipelineCreateInfo*>(record.info);
        if (Code code = StageProgram(source, ci.stage)) {
            programs.push_back({ci.stage.stage, std::move(code)});
        }
        break;
    }
    case DriverCall::Kind::GraphicsPipeline:
        programs = GraphicsPrograms(source,
                                    *static_cast<const VkGraphicsPipelineCreateInfo*>(record.info));
        break;
    default:
        break;
    }
    return programs;
}

std::string_view Trim(std::string_view name, std::string_view prefix) {
    if (name.starts_with(prefix)) {
        name.remove_prefix(prefix.size());
    }
    if (name.ends_with("_BIT")) {
        name.remove_suffix(4);
    }
    return name;
}

void WriteNextChain(std::FILE* file, const char* indent, const void* next) {
    for (auto* header = static_cast<const VkBaseInStructure*>(next); header != nullptr;
         header = header->pNext) {
        fmt::print(file, "{}pNext {}\n", indent, string_VkStructureType(header->sType));
    }
}

void WriteStage(std::FILE* file, const VkPipelineShaderStageCreateInfo& stage) {
    fmt::print(file, "stage {} flags={:#x} module={:#x} entry={}\n",
               Trim(string_VkShaderStageFlagBits(stage.stage), "VK_SHADER_STAGE_"),
               static_cast<u32>(stage.flags), reinterpret_cast<uintptr_t>(stage.module),
               stage.pName != nullptr ? stage.pName : "(null)");
    WriteNextChain(file, "  ", stage.pNext);
    if (const VkSpecializationInfo* spec = stage.pSpecializationInfo) {
        for (u32 i = 0; i < spec->mapEntryCount; ++i) {
            const VkSpecializationMapEntry& entry = spec->pMapEntries[i];
            u64 value{};
            if (entry.offset + entry.size <= spec->dataSize && entry.size <= sizeof(value)) {
                std::memcpy(&value, static_cast<const u8*>(spec->pData) + entry.offset, entry.size);
            }
            fmt::print(file, "  spec constant {} = {:#x} ({} bytes)\n", entry.constantID, value,
                       entry.size);
        }
    }
}

void WriteGraphicsState(std::FILE* file, const VkGraphicsPipelineCreateInfo& ci) {
    fmt::print(file, "flags={:#x} layout={:#x} renderPass={:#x} subpass={}\n",
               static_cast<u32>(ci.flags), reinterpret_cast<uintptr_t>(ci.layout),
               reinterpret_cast<uintptr_t>(ci.renderPass), ci.subpass);
    WriteNextChain(file, "", ci.pNext);
    if (const auto* flags2 = FindNext<VkPipelineCreateFlags2CreateInfo>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)) {
        fmt::print(file, "flags2={:#x}\n", static_cast<u64>(flags2->flags));
    }
    if (const auto* library = FindNext<VkGraphicsPipelineLibraryCreateInfoEXT>(
            ci.pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)) {
        fmt::print(file, "library parts={:#x}\n", static_cast<u32>(library->flags));
    }
    if (const auto* link = FindNext<VkPipelineLibraryCreateInfoKHR>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)) {
        for (u32 i = 0; i < link->libraryCount; ++i) {
            fmt::print(file, "linked library {:#x}\n",
                       reinterpret_cast<uintptr_t>(link->pLibraries[i]));
        }
    }
    if (const auto* rendering = FindNext<VkPipelineRenderingCreateInfo>(
            ci.pNext, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)) {
        fmt::print(file, "rendering viewMask={:#x} depth={} stencil={}\n", rendering->viewMask,
                   string_VkFormat(rendering->depthAttachmentFormat),
                   string_VkFormat(rendering->stencilAttachmentFormat));
        for (u32 i = 0; i < rendering->colorAttachmentCount; ++i) {
            fmt::print(file, "  color {} {}\n", i,
                       string_VkFormat(rendering->pColorAttachmentFormats[i]));
        }
    }
    for (u32 i = 0; i < ci.stageCount && ci.pStages != nullptr; ++i) {
        WriteStage(file, ci.pStages[i]);
    }
    if (const auto* vi = ci.pVertexInputState) {
        for (u32 i = 0; i < vi->vertexBindingDescriptionCount; ++i) {
            const auto& b = vi->pVertexBindingDescriptions[i];
            fmt::print(file, "vertex binding {} stride={} rate={}\n", b.binding, b.stride,
                       string_VkVertexInputRate(b.inputRate));
        }
        for (u32 i = 0; i < vi->vertexAttributeDescriptionCount; ++i) {
            const auto& a = vi->pVertexAttributeDescriptions[i];
            fmt::print(file, "vertex attribute {} binding={} {} offset={}\n", a.location, a.binding,
                       string_VkFormat(a.format), a.offset);
        }
    }
    if (const auto* ia = ci.pInputAssemblyState) {
        fmt::print(file, "input assembly {} restart={}\n", string_VkPrimitiveTopology(ia->topology),
                   ia->primitiveRestartEnable);
    }
    if (const auto* tess = ci.pTessellationState) {
        fmt::print(file, "tessellation patchControlPoints={}\n", tess->patchControlPoints);
    }
    if (const auto* vp = ci.pViewportState) {
        fmt::print(file, "viewports={} scissors={}\n", vp->viewportCount, vp->scissorCount);
    }
    if (const auto* rs = ci.pRasterizationState) {
        fmt::print(file,
                   "rasterization depthClamp={} discard={} {} cull={:#x} {} depthBias={} "
                   "lineWidth={}\n",
                   rs->depthClampEnable, rs->rasterizerDiscardEnable,
                   string_VkPolygonMode(rs->polygonMode), static_cast<u32>(rs->cullMode),
                   string_VkFrontFace(rs->frontFace), rs->depthBiasEnable, rs->lineWidth);
        WriteNextChain(file, "  ", rs->pNext);
    }
    if (const auto* ms = ci.pMultisampleState) {
        fmt::print(file, "multisample samples={} sampleShading={} alphaToCoverage={}\n",
                   static_cast<u32>(ms->rasterizationSamples), ms->sampleShadingEnable,
                   ms->alphaToCoverageEnable);
    }
    if (const auto* ds = ci.pDepthStencilState) {
        fmt::print(file, "depth test={} write={} {} bounds={} stencil={}\n", ds->depthTestEnable,
                   ds->depthWriteEnable, string_VkCompareOp(ds->depthCompareOp),
                   ds->depthBoundsTestEnable, ds->stencilTestEnable);
    }
    if (const auto* cb = ci.pColorBlendState) {
        fmt::print(file, "blend logicOp={} {}\n", cb->logicOpEnable, string_VkLogicOp(cb->logicOp));
        for (u32 i = 0; i < cb->attachmentCount && cb->pAttachments != nullptr; ++i) {
            const auto& a = cb->pAttachments[i];
            fmt::print(file, "  attachment {} enable={} color {} {} {} alpha {} {} {} mask={:#x}\n",
                       i, a.blendEnable, string_VkBlendFactor(a.srcColorBlendFactor),
                       string_VkBlendFactor(a.dstColorBlendFactor),
                       string_VkBlendOp(a.colorBlendOp),
                       string_VkBlendFactor(a.srcAlphaBlendFactor),
                       string_VkBlendFactor(a.dstAlphaBlendFactor),
                       string_VkBlendOp(a.alphaBlendOp), static_cast<u32>(a.colorWriteMask));
        }
    }
    if (const auto* dyn = ci.pDynamicState) {
        for (u32 i = 0; i < dyn->dynamicStateCount; ++i) {
            fmt::print(file, "dynamic {}\n", string_VkDynamicState(dyn->pDynamicStates[i]));
        }
    }
}

std::atomic<u32> artifact_count{};

#ifdef _WIN32
bool IsFault(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return true;
    default:
        return false;
    }
}

/// First chance, before the driver's own handlers: a fault inside a marked driver call. Fastmem
/// faults happen on guest threads outside any driver call and pass through untouched.
LONG CALLBACK OnException(EXCEPTION_POINTERS* pointers) {
    const DriverCall::Record* record = DriverCall::Current();
    const EXCEPTION_RECORD& exception = *pointers->ExceptionRecord;
    if (record == nullptr || !IsFault(exception.ExceptionCode) ||
        DriverCall::FaultArtifact(record) != nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HMODULE module{};
    char module_name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(exception.ExceptionAddress), &module)) {
        GetModuleFileNameA(module, module_name, sizeof(module_name));
    }
    const char* base_name = std::strrchr(module_name, '\\');
    const auto offset =
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exception.ExceptionAddress) -
                                        reinterpret_cast<uintptr_t>(module));
    const auto reason =
        fmt::format("Exception {:#x} at {}+{:#x} on thread {}, info {:#x} {:#x}",
                    static_cast<u32>(exception.ExceptionCode),
                    base_name ? base_name + 1 : module_name, offset, GetCurrentThreadId(),
                    exception.NumberParameters > 0 ? exception.ExceptionInformation[0] : 0,
                    exception.NumberParameters > 1 ? exception.ExceptionInformation[1] : 0);
    const std::string folder =
        WriteArtifact(Directory(), *record, DriverCall::CurrentContext(), reason.c_str());
    DriverCall::NoteFault(record, folder);
    LOG_CRITICAL(Render_Vulkan, "Driver crash in {}: {} (repro: {})", DriverCall::DescribeCurrent(),
                 reason, folder.empty() ? "not written" : folder);
    Common::Log::Stop();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

} // Anonymous namespace

const std::string& Directory() {
    static const std::string directory = [] {
        const char* value = std::getenv("EDEN_NX2_CRASH_DUMP");
        std::string result = value != nullptr ? value : "";
#ifdef _WIN32
        if (!result.empty()) {
            AddVectoredExceptionHandler(1, OnException);
        }
#endif
        return result;
    }();
    return directory;
}

void RegisterModule(VkShaderModule module, std::span<const u32> code) {
    Registry& registry = GetRegistry();
    auto copy = std::make_shared<const std::vector<u32>>(code.begin(), code.end());
    std::scoped_lock lock{registry.mutex};
    registry.modules.insert_or_assign(module, std::move(copy));
}

void ForgetModule(VkShaderModule module) {
    Registry& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    registry.modules.erase(module);
}

void RegisterPipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo& ci) {
    // Libraries, so a link can name them; linked pipelines are not linked again.
    if (!IsLibrary(ci)) {
        return;
    }
    Registry& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    std::vector<StageCode> programs = GraphicsPrograms(registry, ci);
    if (!programs.empty()) {
        registry.pipelines.insert_or_assign(pipeline, std::move(programs));
    }
}

void ForgetPipeline(VkPipeline pipeline) {
    Registry& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    registry.pipelines.erase(pipeline);
}

void WriteState(std::FILE* file, const DriverCall::Record& record) {
    if (record.info == nullptr) {
        return;
    }
    switch (record.kind) {
    case DriverCall::Kind::GraphicsPipeline:
        WriteGraphicsState(file, *static_cast<const VkGraphicsPipelineCreateInfo*>(record.info));
        break;
    case DriverCall::Kind::ComputePipeline: {
        const auto& ci = *static_cast<const VkComputePipelineCreateInfo*>(record.info);
        fmt::print(file, "flags={:#x} layout={:#x}\n", static_cast<u32>(ci.flags),
                   reinterpret_cast<uintptr_t>(ci.layout));
        WriteNextChain(file, "", ci.pNext);
        WriteStage(file, ci.stage);
        break;
    }
    case DriverCall::Kind::Shaders: {
        const auto* infos = static_cast<const VkShaderCreateInfoEXT*>(record.info);
        for (u32 i = 0; i < record.count; ++i) {
            const VkShaderCreateInfoEXT& ci = infos[i];
            fmt::print(file,
                       "shader {} {} flags={:#x} nextStage={:#x} bytes={} setLayouts={} "
                       "pushConstantRanges={} entry={}\n",
                       i, Trim(string_VkShaderStageFlagBits(ci.stage), "VK_SHADER_STAGE_"),
                       static_cast<u32>(ci.flags), static_cast<u32>(ci.nextStage), ci.codeSize,
                       ci.setLayoutCount, ci.pushConstantRangeCount,
                       ci.pName != nullptr ? ci.pName : "(null)");
            WriteNextChain(file, "  ", ci.pNext);
        }
        break;
    }
    default: {
        std::array<char, 1024> line;
        const size_t length = DriverCall::Describe(line.data(), line.size(), &record, nullptr);
        fmt::print(file, "{}\n", std::string_view(line.data(), length));
        break;
    }
    }
}

std::string WriteArtifact(const std::string& directory, const DriverCall::Record& record,
                          const DriverCall::Context* context, const char* reason) {
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const auto folder = std::filesystem::path{directory} /
                        fmt::format("driver-crash-{}-{}", pid, artifact_count.fetch_add(1));
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        return {};
    }
    bool registry_busy{};
    const std::vector<StageCode> programs = Programs(record, registry_busy);
    std::FILE* summary = std::fopen((folder / "summary.txt").string().c_str(), "w");
    if (summary == nullptr) {
        return {};
    }
    fmt::print(summary, "{}\n", reason);
    for (const DriverCall::Record* call = &record; call != nullptr; call = call->outer) {
        std::array<char, 1024> line;
        const size_t length = DriverCall::Describe(line.data(), line.size(), call,
                                                   call == &record ? context : nullptr);
        fmt::print(summary, "{} {}\n", call == &record ? "in" : "within",
                   std::string_view(line.data(), length));
    }
    for (size_t i = 0; i < programs.size(); ++i) {
        const auto& code = *programs[i].code;
        const auto name = fmt::format(
            "{}-{}-{:016x}.spv", i,
            programs[i].stage == VK_SHADER_STAGE_ALL
                ? std::string_view{"module"}
                : Trim(string_VkShaderStageFlagBits(programs[i].stage), "VK_SHADER_STAGE_"),
            DriverCall::SpirvHash(code));
        if (std::FILE* spv = std::fopen((folder / name).string().c_str(), "wb")) {
            std::fwrite(code.data(), sizeof(u32), code.size(), spv);
            std::fclose(spv);
        }
        fmt::print(summary, "spirv {}\n", name);
    }
    if (registry_busy) {
        fmt::print(summary, "registry busy: only inline SPIR-V written\n");
    }
    std::fclose(summary);
    if (std::FILE* state = std::fopen((folder / "state.txt").string().c_str(), "w")) {
        WriteState(state, record);
        std::fclose(state);
    }
    return folder.string();
}

} // namespace Vulkan::DriverCrash
