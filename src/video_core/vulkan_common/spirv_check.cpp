// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <unordered_set>

#include <fmt/format.h>

#include "common/dynamic_library.h"
#include "common/logging.h"
#include "video_core/vulkan_common/driver_call.h"
#include "video_core/vulkan_common/spirv_check.h"
#include "video_core/vulkan_common/vulkan.h"

#ifdef _WIN32
#include "common/fs/path_util.h"
#endif

namespace Vulkan::SpirvCheck {
namespace {

// SPIRV-Tools' C API (libspirv.h), the parts used here. Its enums only ever grow.
struct SpvContext;
struct SpvValidatorOptions;
struct SpvConstBinary {
    const u32* code;
    size_t word_count;
};
struct SpvPosition {
    size_t line;
    size_t column;
    size_t index;
};
struct SpvDiagnostic {
    SpvPosition position;
    char* error;
    bool is_text_source;
};
struct SpvText {
    const char* str;
    size_t length;
};
constexpr int SpvSuccess = 0;
constexpr u32 SpvBinaryToTextIndent = 1u << 3;
constexpr u32 SpvBinaryToTextFriendlyNames = 1u << 6;
constexpr u32 SpvBinaryToTextComment = 1u << 7;

struct Api {
    Common::DynamicLibrary library;
    std::string path;
    SpvContext* (*context_create)(int env){};
    void (*context_destroy)(SpvContext*){};
    SpvValidatorOptions* (*options_create)(){};
    void (*options_destroy)(SpvValidatorOptions*){};
    void (*options_relax_block_layout)(SpvValidatorOptions*, bool){};
    void (*options_uniform_buffer_standard_layout)(SpvValidatorOptions*, bool){};
    void (*options_scalar_block_layout)(SpvValidatorOptions*, bool){};
    void (*options_workgroup_scalar_block_layout)(SpvValidatorOptions*, bool){};
    void (*options_allow_local_size_id)(SpvValidatorOptions*, bool){};
    int (*validate)(const SpvContext*, const SpvValidatorOptions*, const SpvConstBinary*,
                    SpvDiagnostic**){};
    void (*diagnostic_destroy)(SpvDiagnostic*){};
    int (*binary_to_text)(const SpvContext*, const u32*, size_t, u32, SpvText**, SpvDiagnostic**){};
    void (*text_destroy)(SpvText*){};

    bool Load(const std::string& candidate) {
        if (!library.Open(candidate.c_str())) {
            return false;
        }
        path = candidate;
        return library.GetSymbol("spvContextCreate", &context_create) &&
               library.GetSymbol("spvContextDestroy", &context_destroy) &&
               library.GetSymbol("spvValidatorOptionsCreate", &options_create) &&
               library.GetSymbol("spvValidatorOptionsDestroy", &options_destroy) &&
               library.GetSymbol("spvValidatorOptionsSetRelaxBlockLayout",
                                 &options_relax_block_layout) &&
               library.GetSymbol("spvValidatorOptionsSetUniformBufferStandardLayout",
                                 &options_uniform_buffer_standard_layout) &&
               library.GetSymbol("spvValidatorOptionsSetScalarBlockLayout",
                                 &options_scalar_block_layout) &&
               library.GetSymbol("spvValidatorOptionsSetWorkgroupScalarBlockLayout",
                                 &options_workgroup_scalar_block_layout) &&
               library.GetSymbol("spvValidatorOptionsSetAllowLocalSizeId",
                                 &options_allow_local_size_id) &&
               library.GetSymbol("spvValidateWithOptions", &validate) &&
               library.GetSymbol("spvDiagnosticDestroy", &diagnostic_destroy) &&
               library.GetSymbol("spvBinaryToText", &binary_to_text) &&
               library.GetSymbol("spvTextDestroy", &text_destroy);
    }
};

struct State {
    std::mutex mutex;
    std::optional<Api> api;
    Options options{};
    std::unordered_set<u64> reported;
};

State& GetState() {
    static State state;
    return state;
}

/// One context and option set per thread: SPIRV-Tools does not promise either is safe to share.
struct ThreadValidator {
    const Api* api{};
    SpvContext* context{};
    SpvValidatorOptions* options{};

    ~ThreadValidator() {
        if (api != nullptr) {
            api->options_destroy(options);
            api->context_destroy(context);
        }
    }
};

ThreadValidator& Validator(const Api& api, const Options& options) {
    thread_local ThreadValidator validator;
    if (validator.api != &api) {
        validator.api = &api;
        validator.context = api.context_create(TargetEnv(options.api_version));
        validator.options = api.options_create();
        api.options_relax_block_layout(validator.options, true);
        api.options_uniform_buffer_standard_layout(validator.options,
                                                   options.uniform_buffer_standard_layout);
        api.options_scalar_block_layout(validator.options, options.scalar_block_layout);
        api.options_workgroup_scalar_block_layout(validator.options,
                                                  options.workgroup_scalar_block_layout);
        api.options_allow_local_size_id(validator.options, options.local_size_id);
    }
    return validator;
}

std::vector<std::string> Candidates() {
    std::vector<std::string> candidates;
    if (const char* path = std::getenv("EDEN_NX2_SPIRV_TOOLS"); path != nullptr && *path) {
        candidates.emplace_back(path);
    }
#ifdef _WIN32
    constexpr const char* name = "SPIRV-Tools-shared.dll";
#elif defined(__APPLE__)
    constexpr const char* name = "libSPIRV-Tools-shared.dylib";
#else
    constexpr const char* name = "libSPIRV-Tools-shared.so";
#endif
#ifdef _WIN32
    candidates.push_back((Common::FS::GetExeDirectory() / name).string());
#endif
    candidates.emplace_back(name);
    return candidates;
}

} // Anonymous namespace

const std::string& Directory() {
    static const std::string directory = [] {
        const char* value = std::getenv("EDEN_NX2_SPIRV_VALIDATE");
        return std::string{value != nullptr ? value : ""};
    }();
    return directory;
}

int TargetEnv(u32 api_version) {
    // spv_target_env: SPV_ENV_VULKAN_1_0 = 1, _1_1 = 18, _1_2 = 23, _1_3 = 25, _1_4 = 26.
    const u32 minor = VK_API_VERSION_MINOR(api_version);
    if (minor >= 4) {
        return 26;
    }
    switch (minor) {
    case 3:
        return 25;
    case 2:
        return 23;
    case 1:
        return 18;
    default:
        return 1;
    }
}

void Configure(const Options& options) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    state.options = options;
    if (!state.api) {
        for (const std::string& candidate : Candidates()) {
            Api api;
            if (api.Load(candidate)) {
                state.api.emplace(std::move(api));
                break;
            }
        }
    }
    if (!state.api) {
        LOG_ERROR(Render_Vulkan,
                  "SPIR-V check: SPIRV-Tools-shared not found (set EDEN_NX2_SPIRV_TOOLS to its "
                  "path); shaders are not validated");
        return;
    }
    LOG_INFO(Render_Vulkan, "SPIR-V check: validating every module with {} for Vulkan 1.{}",
             state.api->path, VK_API_VERSION_MINOR(options.api_version));
}

bool Ready() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    return state.api.has_value();
}

std::string Validate(std::span<const u32> code) {
    State& state = GetState();
    const Api* api;
    Options options;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.api) {
            return {};
        }
        api = &*state.api;
        options = state.options;
    }
    ThreadValidator& validator = Validator(*api, options);
    const SpvConstBinary binary{code.data(), code.size()};
    SpvDiagnostic* diagnostic{};
    const int result = api->validate(validator.context, validator.options, &binary, &diagnostic);
    std::string message;
    if (result != SpvSuccess) {
        message = diagnostic != nullptr && diagnostic->error != nullptr
                      ? std::string{diagnostic->error}
                      : fmt::format("spv_result_t {}", result);
    }
    if (diagnostic != nullptr) {
        api->diagnostic_destroy(diagnostic);
    }
    return message;
}

void Check(std::span<const u32> code) {
    std::string message = Validate(code);
    if (message.empty()) {
        return;
    }
    const u64 hash = DriverCall::SpirvHash(code);
    State& state = GetState();
    const Api* api;
    Options options;
    {
        std::scoped_lock lock{state.mutex};
        if (!state.reported.insert(hash).second) {
            return;
        }
        api = &*state.api;
        options = state.options;
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    const std::string context = DriverCall::DescribeCurrent();
    const auto stem =
        std::filesystem::path{Directory()} / fmt::format("spirv-invalid-{:016x}", hash);
    std::error_code error;
    std::filesystem::create_directories(Directory(), error);
    if (std::FILE* file = std::fopen((stem.string() + ".spv").c_str(), "wb")) {
        std::fwrite(code.data(), sizeof(u32), code.size(), file);
        std::fclose(file);
    }
    if (std::FILE* file = std::fopen((stem.string() + ".txt").c_str(), "w")) {
        fmt::print(file, "{}\n{}\n", context, message);
        std::fclose(file);
    }
    ThreadValidator& validator = Validator(*api, options);
    SpvText* text{};
    SpvDiagnostic* diagnostic{};
    if (api->binary_to_text(validator.context, code.data(), code.size(),
                            SpvBinaryToTextIndent | SpvBinaryToTextFriendlyNames |
                                SpvBinaryToTextComment,
                            &text, &diagnostic) == SpvSuccess &&
        text != nullptr) {
        if (std::FILE* file = std::fopen((stem.string() + ".spvasm").c_str(), "w")) {
            std::fwrite(text->str, 1, text->length, file);
            std::fclose(file);
        }
    }
    if (text != nullptr) {
        api->text_destroy(text);
    }
    if (diagnostic != nullptr) {
        api->diagnostic_destroy(diagnostic);
    }
    LOG_ERROR(Render_Vulkan, "SPIR-V check: module {:016x} is invalid{}{}: {} (dump {}.spv)", hash,
              context.empty() ? "" : " in ", context, message, stem.string());
}

} // namespace Vulkan::SpirvCheck
