// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/shader_archive.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/trap_guards.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/offline_environment.h"
#include "shader_recompiler/offline_graphics.h"
#include "shader_recompiler/nvn.h"

namespace {
using Json = nlohmann::json;
using Shader::Stage;
using Shader::Ampere::Instruction;

using MissingContext = Shader::MissingOfflineContext;
using OfflineEnvironment = Shader::OfflineEnvironment;
using ArchiveGroup = std::array<std::optional<Shader::Ampere::ArchiveProgram>, 9>;
using ArchiveGroups = std::map<std::pair<size_t, u32>, ArchiveGroup>;

constexpr std::array StageNames{"vertex",  "tess-control", "tess-eval", "geometry", "fragment",
                                "compute", "vertex-a",     "task",      "mesh"};

Stage ParseStage(std::string_view name) {
    for (size_t i = 0; i < StageNames.size(); ++i) {
        if (name == StageNames[i])
            return static_cast<Stage>(i);
    }
    throw std::runtime_error("Unknown stage: " + std::string{name});
}

std::vector<u8> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open " + path.string());
    const auto size = file.tellg();
    if (size < 0 || size > 64 * 1024 * 1024) {
        throw std::runtime_error("Input exceeds the 64 MiB limit");
    }
    std::vector<u8> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Cannot read " + path.string());
    }
    return bytes;
}

template <typename T>
void WriteFile(const std::filesystem::path& path, const std::vector<T>& words) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file ||
        !file.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(T))) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    file.close();
    if (!file)
        throw std::runtime_error("Cannot close " + path.string());
}

Shader::RuntimeInfo Runtime(std::span<const u8> key, Stage stage, Json& assumptions) {
    Shader::RuntimeInfo info{};
    info.previous_stage_stores.mask.set();
    assumptions.push_back(
        "Sibling stage linkage is unavailable; all previous-stage varyings are enabled");
    if (key.empty()) {
        assumptions.push_back("No pipeline key: point input topology, isolines/equal tessellation, "
                              "float attributes, no alpha test or transform feedback");
        return info;
    }
    if (key.size() != 1056 || stage == Stage::Compute) {
        throw std::runtime_error("Expected a 1056-byte graphics pipeline key");
    }
    const auto word = [&](size_t offset) {
        u32 value;
        std::memcpy(&value, key.data() + offset, sizeof(value));
        return value;
    };
    const u32 raw1 = word(48), raw2 = word(52);
    if (raw1 & (1U << 6))
        throw MissingContext("Transform feedback pipeline state is not supported offline");
    const u32 topology = (raw1 >> 24) & 15;
    switch (topology) {
    case 0:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case 1:
    case 2:
    case 3:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 14:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    case 10:
    case 11:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case 12:
    case 13:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    default:
        throw std::runtime_error("Invalid pipeline input topology");
    }
    if (stage == Stage::TessellationEval) {
        const u32 primitive = (raw1 >> 10) & 3, spacing = (raw1 >> 12) & 3;
        if (primitive > 2 || spacing > 2)
            throw std::runtime_error("Invalid tessellation state");
        info.tess_primitive = static_cast<Shader::TessPrimitive>(primitive);
        info.tess_spacing = static_cast<Shader::TessSpacing>(spacing);
        info.tess_clockwise = (raw1 >> 14) & 1;
    }
    info.force_early_z = (raw2 >> 4) & 1;
    info.y_negate = (raw2 >> 11) & 1;
    if (stage == Stage::Geometry || (stage == Stage::VertexB && word(32) == 0 && word(36) == 0)) {
        info.convert_depth_mode = (raw1 >> 7) & 1;
        if (topology == 0)
            info.fixed_state_point_size = std::bit_cast<float>(word(76));
    }
    if (stage == Stage::Fragment) {
        info.alpha_test_func = static_cast<Shader::CompareFunction>((raw2 >> 1) & 7);
        info.alpha_test_reference = std::bit_cast<float>(word(72));
        info.dual_source_blend = (raw1 >> 21) & 1;
        for (size_t index = 0; index < info.color_output_types.size(); ++index) {
            switch (key[56 + index]) {
            case 0xc1:
            case 0xc4:
            case 0xc8:
            case 0xcc:
            case 0xd8:
            case 0xdc:
            case 0xe3:
            case 0xec:
            case 0xf0:
            case 0xf5:
                info.color_output_types[index] = Shader::AttributeType::SignedInt;
                break;
            case 0xc2:
            case 0xc5:
            case 0xc9:
            case 0xcd:
            case 0xd2:
            case 0xd9:
            case 0xdd:
            case 0xe4:
            case 0xed:
            case 0xf1:
            case 0xf6:
                info.color_output_types[index] = Shader::AttributeType::UnsignedInt;
                break;
            case 0:
            case 0xc0:
            case 0xc3:
            case 0xc6:
            case 0xc7:
            case 0xca:
            case 0xcb:
            case 0xce:
            case 0xcf:
            case 0xd0:
            case 0xd1:
            case 0xd5:
            case 0xd6:
            case 0xd7:
            case 0xda:
            case 0xdb:
            case 0xde:
            case 0xdf:
            case 0xe0:
            case 0xe5:
            case 0xe6:
            case 0xe7:
            case 0xe8:
            case 0xe9:
            case 0xea:
            case 0xeb:
            case 0xee:
            case 0xef:
            case 0xf2:
            case 0xf3:
            case 0xf4:
            case 0xf8:
            case 0xf9:
            case 0xfa:
                info.color_output_types[index] = Shader::AttributeType::Float;
                break;
            default:
                throw std::runtime_error("Invalid pipeline color attachment format");
            }
        }
    }
    if (stage == Stage::VertexB) {
        for (size_t index = 0; index < info.generic_input_types.size(); ++index) {
            auto& type = info.generic_input_types[index];
            if (raw1 & (1U << 5)) {
                constexpr std::array types{
                    Shader::AttributeType::Disabled, Shader::AttributeType::Float,
                    Shader::AttributeType::SignedInt, Shader::AttributeType::UnsignedInt};
                type = types[(word(112 + (index / 16) * 4) >> ((index % 16) * 2)) & 3];
                continue;
            }
            const u32 attribute = word(160 + index * 4);
            if (!(attribute & 1)) {
                type = Shader::AttributeType::Disabled;
                continue;
            }
            switch ((attribute >> 20) & 7) {
            case 1:
            case 2:
            case 7:
                type = Shader::AttributeType::Float;
                break;
            case 3:
                type = Shader::AttributeType::SignedInt;
                break;
            case 4:
                type = Shader::AttributeType::UnsignedInt;
                break;
            case 5:
                type = Shader::AttributeType::UnsignedScaled;
                break;
            case 6:
                type = Shader::AttributeType::SignedScaled;
                break;
            default:
                throw std::runtime_error("Invalid pipeline vertex attribute type");
            }
        }
    }
    assumptions.push_back("Pipeline key supplies vertex/fragment interface types and fixed state; "
                          "integer fragment attachments use typed outputs on every offline host");
    return info;
}

Shader::HostTranslateInfo OfflineHost() {
    Shader::HostTranslateInfo host{};
    host.support_float64 = true;
    host.support_float16 = true;
    host.support_int64 = true;
    host.support_conditional_barrier = true;
    host.support_viewport_index_layer = true;
    host.support_geometry_shader_passthrough = true;
    host.ApplyDescriptorLimitPolicy();
    return host;
}

Shader::Profile OfflineProfile(const Shader::HostTranslateInfo& host) {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010400;
    profile.unified_descriptor_binding = true;
    profile.min_ssbo_alignment = host.min_ssbo_alignment;
    profile.support_int64 = true;
    profile.support_int16 = true;
    profile.support_int8 = true;
    profile.support_vote = true;
    profile.support_shuffle_relative = true;
    profile.support_geometry_streams = true;
    profile.support_geometry_shader_passthrough = true;
    profile.support_viewport_index_layer_non_geometry = true;
    profile.support_multi_viewport = true;
    profile.max_user_clip_distances = 8;
    profile.max_subgroup_size = 32;
    profile.support_typeless_image_loads = true;
    return profile;
}

Json ProbeGraphics(const ArchiveGroups& groups, const std::filesystem::path& output,
                   bool float_textures) {
    if (std::filesystem::exists(output)) {
        throw std::runtime_error("Graphics probe output directory must not exist");
    }
    std::filesystem::create_directories(output);
    const auto host = OfflineHost();
    const auto profile = OfflineProfile(host);
    Json results = Json::array();
    size_t compiled = 0;
    for (const auto& [identity, group] : groups) {
        Json result{{"container_offset", identity.first}, {"variation", identity.second},
                    {"status", "deferred"}};
        try {
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
            Shader::ObjectPool<Shader::IR::Inst> inst_pool;
            Shader::ObjectPool<Shader::IR::Block> block_pool;
            Shader::Backend::Bindings bindings{};
            std::optional<Shader::IR::Program> previous;
            std::vector<std::pair<size_t, std::vector<u32>>> modules;
            Json constants = Json::array();
            Json textures = Json::array();
            Json layouts = Json::array();
            std::vector<Shader::Ampere::ArchiveProgram> source_group;
            for (const auto& stage : group) if (stage) source_group.push_back(*stage);
            const auto layout = Shader::GetOfflineGraphicsLayout(source_group);
            if (layout.mesh) {
                result["mesh_contract"] = {{"topology", "triangles (test contract, not inferred)"},
                    {"threads", layout.mesh->threads}, {"payload_bytes", layout.mesh->payload_bytes},
                    {"max_vertices", layout.mesh->max_vertices}, {"max_primitives", layout.mesh->max_primitives}};
            }
            for (size_t index = 0; index < layout.count; ++index) {
                const auto& source = *layout.programs[index];
                const size_t stage = static_cast<size_t>(source.stage);
                const auto native = Shader::CaptureOfflineGraphics(source);
                const auto lowered = Shader::Ampere::LowerToMaxwell(native, {.tensor = true});
                auto env = Shader::MakeOfflineGraphicsEnvironment(source, layout, lowered, float_textures);
                Shader::Maxwell::Flow::CFG cfg{env, flow_pool,
                    Shader::Maxwell::Location{env.EntryPoint()}};
                auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
                if (!program.info.handle_programs.empty()) {
                    throw MissingContext("Graphics probe requires runtime-computed texture handles");
                }
                Shader::RuntimeInfo runtime{};
                runtime.input_topology = Shader::InputTopology::Triangles;
                runtime.alpha_test_func = Shader::CompareFunction::Always;
                runtime.mesh_has_task = previous && previous->stage == Stage::Task;
                if (previous) {
                    runtime.previous_stage_stores = previous->info.stores;
                    runtime.previous_stage_legacy_stores_mapping = previous->info.legacy_stores_mapping;
                    if (previous->stage == Stage::Mesh) {
                        for (const u32 line : previous->mesh.primitive_lines) {
                            const auto attribute = static_cast<Shader::IR::Attribute>(line);
                            if (Shader::IR::IsGeneric(attribute)) {
                                runtime.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
                            }
                        }
                    }
                } else {
                    runtime.previous_stage_stores.mask.set();
                }
                Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
                Json layout = Json::array();
                u32 next_binding = bindings.unified;
                const auto add_layout = [&](const auto& descriptors, const char* kind) {
                    for (const auto& descriptor : descriptors) {
                        if (descriptor.count != 1) {
                            throw MissingContext("Driver probe requires single descriptors");
                        }
                        layout.push_back({{"binding", next_binding++}, {"kind", kind}, {"count", 1}});
                    }
                };
                add_layout(program.info.constant_buffer_descriptors, "uniform_buffer");
                add_layout(program.info.storage_buffers_descriptors, "storage_buffer");
                add_layout(program.info.texture_buffer_descriptors, "uniform_texel_buffer");
                add_layout(program.info.image_buffer_descriptors, "storage_texel_buffer");
                add_layout(program.info.texture_descriptors, "combined_image_sampler");
                add_layout(program.info.image_descriptors, "storage_image");
                modules.emplace_back(stage, Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings));
                if (next_binding != bindings.unified) {
                    throw MissingContext("Driver probe layout disagrees with emitted bindings");
                }
                const auto& info = program.info;
                const bool warp_ops = info.uses_subgroup_vote || info.uses_subgroup_invocation_id ||
                    info.uses_subgroup_shuffles || info.uses_subgroup_mask || info.uses_quad_shuffles ||
                    info.uses_fswzadd;
                layouts.push_back({{"stage", StageNames[stage]}, {"bindings", std::move(layout)},
                                   {"uses_warp_ops", warp_ops}});
                Json stage_constants = Json::array();
                for (const auto& [key, value] : env.RequiredConstants()) {
                    stage_constants.push_back({{"bank", key >> 32}, {"offset", u32(key)},
                                               {"value", value}});
                }
                constants.push_back(std::move(stage_constants));
                Json stage_textures = Json::array();
                for (const auto& key : env.RequiredTextures()) {
                    stage_textures.push_back({{"bank", key.index}, {"offset", key.offset},
                        {"shift_left", key.shift_left}, {"secondary_bank", key.sec_index},
                        {"secondary_offset", key.sec_offset}, {"secondary_shift_left", key.sec_shift_left},
                        {"has_secondary", key.has_secondary}, {"count", key.count}});
                }
                textures.push_back(std::move(stage_textures));
                previous = std::move(program);
            }
            const auto name = fmt::format("{:x}-{}", identity.first, identity.second);
            Json paths = Json::array();
            for (const auto& [stage, words] : modules) {
                const auto path = name + "-" + StageNames[stage] + ".spv";
                WriteFile(output / path, words);
                paths.push_back(path);
            }
            result["status"] = "compiled_under_contract";
            result["modules"] = std::move(paths);
            result["constant_dependencies"] = std::move(constants);
            result["texture_dependencies"] = std::move(textures);
            result["descriptor_layouts"] = std::move(layouts);
            ++compiled;
        } catch (const std::exception& error) {
            result["reason"] = error.what();
        }
        results.push_back(std::move(result));
    }
    Json report{{"schema_version", 2}, {"compiled_groups", compiled}, {"groups", results.size()},
                {"contract", "SPIR-V 1.4 test profile; float vertex attributes and fragment outputs; "
                    "triangles; no depth conversion, Y-negate, XFB, early-Z or dual-source blending; "
                    "alpha test Always. Actual sibling interfaces and cumulative bindings. "
                    "Not a runtime or final-driver-pipeline cache."}, {"results", std::move(results)}};
    report["texture_contract"] = float_textures
        ? "Direct single C2 descriptors: Color2D, A8B8G8R8_UNORM, non-integer. "
          "Test specialization only; raw handles are not read or invented."
        : "No texture metadata assumed";
    std::ofstream manifest(output / "report.json");
    manifest << report.dump(2) << '\n';
    if (!manifest) throw std::runtime_error("Cannot save graphics probe report");
    return {{"compiled_groups", compiled}, {"groups", groups.size()},
            {"report", (output / "report.json").string()}};
}
} // namespace

int main(int argc, char** argv) {
    Json diagnostic{{"schema_version", 1}, {"status", "error"},           {"phase", "input"},
                    {"stage", nullptr},    {"input_format", nullptr},     {"source_bytes", 0},
                    {"spirv_words", 0},    {"assumptions", Json::array()}};
    try {
        const bool graphics_probe = (argc == 4 || argc == 5) && std::string_view{argv[1]} == "--graphics-probe";
        const bool float_textures = graphics_probe && argc == 5 && std::string_view{argv[4]} == "--float-textures";
        if (graphics_probe && argc == 5 && !float_textures) {
            throw std::runtime_error("Unknown graphics probe option");
        }
        if (graphics_probe || (argc == 3 && std::string_view{argv[1]} == "--archive-info")) {
            const auto bytes = ReadFile(argv[2]);
            std::array<size_t, 9> counts{};
            ArchiveGroups groups;
            size_t unlinked = 0;
            Json mesh_metadata = Json::array();
            Shader::Ampere::ReadShaderArchive(bytes, [&](const auto& program) {
                const size_t stage = static_cast<size_t>(program.stage);
                ++counts[stage];
                if (const auto info = Shader::Ampere::ReadDeclaredMeshInfo(program.code, program.control)) {
                    mesh_metadata.push_back({{"stage", StageNames[stage]},
                        {"code_offset", program.code.data() - bytes.data()},
                        {"threads", info->threads}, {"payload_bytes", info->payload_bytes},
                        {"max_vertices", info->max_vertices}, {"max_primitives", info->max_primitives},
                        {"has_task", info->has_task},
                        {"primitive_header_bytes", info->primitive_header.size()}});
                }
                if (program.variation) {
                    const size_t offset = program.container.data() - bytes.data();
                    auto& slot = groups[{offset, *program.variation}][stage];
                    if (slot) throw std::runtime_error("Duplicate stage in an archive variation");
                    slot = program;
                } else {
                    ++unlinked;
                }
                return true;
            });
            Json linked = Json::array();
            size_t vertex_fragment = 0;
            for (const auto& [identity, group] : groups) {
                Json stages = Json::object();
                for (size_t stage = 0; stage < group.size(); ++stage) {
                    if (!group[stage]) continue;
                    const auto& program = *group[stage];
                    stages[StageNames[stage]] = {
                        {"code_offset", program.code.data() - bytes.data()},
                        {"code_size", program.code.size()},
                        {"control_offset", program.control.data() - bytes.data()},
                        {"control_size", program.control.size()}};
                }
                if (group[0] && group[4]) ++vertex_fragment;
                linked.push_back({{"container_offset", identity.first},
                                  {"variation", identity.second}, {"stages", std::move(stages)}});
            }
            diagnostic["status"] = "ok";
            diagnostic["phase"] = "archive";
            diagnostic["stages"] = counts;
            diagnostic["linked_variations"] = std::move(linked);
            diagnostic["vertex_fragment_variations"] = vertex_fragment;
            diagnostic["unlinked_stage_records"] = unlinked;
            diagnostic["mesh_metadata"] = std::move(mesh_metadata);
            if (graphics_probe) diagnostic["graphics_probe"] = ProbeGraphics(groups, argv[3], float_textures);
            std::cout << diagnostic.dump() << '\n';
            return 0;
        }
        if constexpr (std::endian::native != std::endian::little) {
            throw std::runtime_error("Only little-endian hosts are supported");
        }
        std::string input, output, environment, pipeline_key, control, format{"instructions"};
        std::optional<Stage> requested_stage;
        u32 version{};
        bool lower_only{};
        bool packed_code{};
        bool capture_extent{};
        for (int i = 1; i < argc; ++i) {
            const std::string arg{argv[i]};
            if (arg == "--help") {
                std::cout << "shader-sweep [INPUT] [--stage "
                             "vertex|tess-control|tess-eval|geometry|fragment|compute|task|mesh]\n"
                             "  [--format instructions|native-header] [--environment FILE "
                             "--cache-version 46|47|48|49|50|51|52|53|54]\n"
                             "  [--pipeline-key FILE] [--capture-extent] "
                             "[--control NVN2_CONTROL] "
                             "(--output SPV | --lower-only WORDS | --packed-code WORDS)\n";
                return 0;
            }
            if (!arg.starts_with("--")) {
                if (!input.empty())
                    throw std::runtime_error("Multiple input files");
                input = arg;
                continue;
            }
            if (arg == "--capture-extent") {
                capture_extent = true;
                continue;
            }
            if (++i == argc)
                throw std::runtime_error("Missing value for " + arg);
            const std::string value{argv[i]};
            if (arg == "--stage")
                requested_stage = ParseStage(value);
            else if (arg == "--format")
                format = value;
            else if (arg == "--environment")
                environment = value;
            else if (arg == "--pipeline-key")
                pipeline_key = value;
            else if (arg == "--control")
                control = value;
            else if (arg == "--cache-version") {
                if (value != "46" && value != "47" && value != "48" && value != "49" && value != "50" &&
                    value != "51" && value != "52" && value != "53" && value != "54")
                    throw std::runtime_error("Expected cache version 46 to 54");
                version = static_cast<u32>(std::stoul(value));
            } else if (arg == "--output" || arg == "--lower-only" || arg == "--packed-code") {
                if (!output.empty())
                    throw std::runtime_error("Only one output mode is permitted");
                output = value;
                lower_only = arg != "--output";
                packed_code = arg == "--packed-code";
            } else
                throw std::runtime_error("Unknown option " + arg);
        }
        if (output.empty() || (input.empty() && environment.empty()) ||
            (environment.empty() != (version == 0)) || (lower_only && input.empty()) ||
            (capture_extent && input.empty())) {
            throw std::runtime_error("Invalid arguments; see --help");
        }
        if (format != "instructions" && format != "native-header")
            throw std::runtime_error("Invalid input format");
        if (!control.empty() && (input.empty() || !environment.empty() || lower_only ||
                                (format == "instructions" && requested_stage != Stage::Compute))) {
            throw std::runtime_error("--control requires native input and no captured environment");
        }
        for (const auto& path : {input, environment, pipeline_key, control}) {
            if (!path.empty() && std::filesystem::weakly_canonical(path) ==
                                     std::filesystem::weakly_canonical(output)) {
                throw std::runtime_error("Output must not overwrite an input");
            }
        }
        OfflineEnvironment env;
        if (!environment.empty())
            env.Load(ReadFile(environment), version);
        if (requested_stage) {
            if (env.captured && env.ShaderStage() != *requested_stage)
                throw std::runtime_error("Stage disagrees with environment");
            env.SetStage(*requested_stage);
        }
        diagnostic["input_format"] = input.empty() ? "environment" : format;
        if (!input.empty()) {
            const auto bytes = ReadFile(input);
            const size_t header_size = format == "native-header" ? 128 : 0;
            if (bytes.size() <= header_size) {
                throw std::runtime_error(
                    "Native input must contain 1..65536 complete 16-byte instructions");
            }
            size_t executable_size = bytes.size();
            if (!control.empty()) {
                const auto control_bytes = ReadFile(control);
                const auto sections = Shader::Ampere::ReadArchiveCodeSections(bytes, control_bytes);
                if (!sections || sections->instruction_bytes <= header_size) {
                    throw std::runtime_error("Unsupported native control record or instruction extent");
                }
                executable_size = sections->instruction_bytes;
                env.SetConstantWords(1, sections->constants);
                diagnostic["archive_constant_bytes"] = sections->constants.size();
                if (requested_stage == Stage::Compute) {
                    const auto dimensions = Shader::Ampere::ReadDeclaredComputeWorkgroup(bytes, control_bytes);
                    if (!dimensions) {
                        throw std::runtime_error("Unsupported native compute control record");
                    }
                    env.SetWorkgroup(*dimensions);
                    diagnostic["declared_workgroup"] = *dimensions;
                    if (const auto bindings = Shader::Ampere::ReadDeclaredComputeBindings(control_bytes)) {
                        env.SetDeclaredComputeBindings(*bindings);
                        diagnostic["declared_pointer_slots"] = bindings->pointer_count;
                    }
                }
            }
            if ((executable_size - header_size) % sizeof(Instruction) ||
                (executable_size - header_size) / sizeof(Instruction) > 65536) {
                throw std::runtime_error("Native input must contain 1..65536 complete 16-byte instructions");
            }
            if (header_size) {
                Shader::ProgramHeader header;
                std::memcpy(&header, bytes.data(), sizeof(header));
                const u32 type = header.common0.shader_type;
                if (type < 1 || type > 5)
                    throw std::runtime_error("Invalid native graphics header stage");
                const Stage header_stage = static_cast<Stage>(type - 1);
                const Stage stage = requested_stage.value_or(
                    env.captured ? env.ShaderStage() : header_stage);
                const bool mesh_envelope =
                    (stage == Stage::Task && type == 1) ||
                    (stage == Stage::Mesh && (type == 1 || type == 3));
                if ((stage != header_stage && !mesh_envelope) ||
                    (env.captured && env.ShaderStage() != stage)) {
                    throw std::runtime_error("Stage disagrees with native header");
                }
                env.SetStage(stage);
                env.SetHeader(bytes);
            } else if (!requested_stage && !env.captured && !lower_only) {
                throw std::runtime_error(
                    "Instruction-only input requires --stage or a captured environment");
            }
            if (requested_stage || env.captured || header_size) {
                diagnostic["stage"] = StageNames[static_cast<u32>(env.ShaderStage())];
            }
            diagnostic["source_bytes"] = executable_size - header_size;
            std::vector<Instruction> native((executable_size - header_size) / sizeof(Instruction));
            std::memcpy(native.data(), bytes.data() + header_size,
                        native.size() * sizeof(Instruction));
            if (capture_extent) {
                diagnostic["phase"] = "capture";
                diagnostic["allocation_bytes"] = native.size() * sizeof(Instruction);
                Shader::Ampere::ProgramCaptureState capture;
                bool complete = false;
                for (u32 index = 0; index < native.size(); ++index) {
                    const auto result = capture.Observe(native[index], index);
                    if (result == Shader::Ampere::ProgramCaptureState::Result::InvalidTarget) {
                        throw MissingContext("Native allocation has an out-of-range branch target");
                    }
                    if (result == Shader::Ampere::ProgramCaptureState::Result::Complete) {
                        native.resize(index + 1);
                        complete = true;
                        break;
                    }
                }
                if (!complete) {
                    throw MissingContext("Native allocation lacks a complete runtime capture");
                }
                diagnostic["source_bytes"] = native.size() * sizeof(Instruction);
            }
            diagnostic["phase"] = "lower";
            std::vector<u32> traps;
            for (u32 i = 0; i < native.size(); ++i) {
                if (native[i].Bits(0, 12) == 0x95c)
                    traps.push_back(i * sizeof(Instruction));
            }
            if (!traps.empty()) {
                if (!env.captured)
                    throw MissingContext("Trap guards require captured constant buffers");
                const bool verified = env.ShaderStage() == Stage::Compute &&
                                      Shader::Ampere::VerifyTrapGuards(
                                          std::span{native}.first(traps.back() / 16 + 1),
                                          [&](u32 bank, u32 offset) -> std::optional<u32> {
                                              return env.ReadCbufValue(bank, offset);
                                          });
                if (!verified)
                    throw MissingContext("Native trap guards could not be verified");
            }
            const auto tessellation = env.ShaderStage() == Stage::TessellationControl
                                          ? Shader::Ampere::TessellationStage::Control
                                      : env.ShaderStage() == Stage::TessellationEval
                                          ? Shader::Ampere::TessellationStage::Evaluation
                                          : Shader::Ampere::TessellationStage::None;
            const auto lowered = Shader::Ampere::LowerToMaxwell(
                native, env.ShaderStage() == Stage::Geometry, true, traps, tessellation);
            if (!lowered) {
                diagnostic["error"] = {{"code", "lowering_failed"},
                                       {"message", lowered.error},
                                       {"offset", lowered.error_offset}};
                std::cout << diagnostic.dump() << '\n';
                return 1;
            }
            diagnostic["lowered_words"] = lowered.instructions.size();
            if (!control.empty() && env.ShaderStage() == Stage::Compute) {
                OfflineEnvironment::ValidateComputeMemory(lowered);
            }
            if (lower_only) {
                diagnostic["phase"] = "output";
                if (packed_code) {
                    env.SetCode(lowered);
                    WriteFile(output, env.code);
                } else {
                    WriteFile(output, lowered.instructions);
                }
                diagnostic["status"] = "ok";
                std::cout << diagnostic.dump() << '\n';
                return 0;
            }
            if (!env.captured && !header_size && env.ShaderStage() != Stage::Compute) {
                throw MissingContext(
                    "Graphics compilation requires a native header or captured environment");
            }
            env.SetCode(lowered);
            if (!env.captured) {
                diagnostic["assumptions"].push_back(control.empty()
                    ? "No environment: workgroup 1x1x1, shared memory 0, viewport enabled, no HLE replacements"
                    : "Archive C1 constants and declared compute workgroup; runtime resource values unavailable");
                if (!control.empty() && env.ShaderStage() == Stage::Compute) {
                    diagnostic["assumptions"].push_back(
                        "C0 resource declarations classify pointer slots; current GPU mapping is not verified");
                }
                if (!header_size && control.empty())
                    diagnostic["assumptions"].push_back(
                        "Compute local memory defaults to zero without a captured environment");
            }
        }
        diagnostic["stage"] = StageNames[static_cast<u32>(env.ShaderStage())];
        if (env.ShaderStage() == Stage::Task || env.ShaderStage() == Stage::Mesh ||
            env.ShaderStage() == Stage::VertexA) {
            throw MissingContext(
                "Task, mesh and vertex-A require additional pipeline stage context");
        }
        const auto key = pipeline_key.empty() ? std::vector<u8>{} : ReadFile(pipeline_key);
        auto runtime = Runtime(key, env.ShaderStage(), diagnostic["assumptions"]);
        diagnostic["assumptions"].push_back(
            "Offline SPIR-V 1.4 profile; no device feature probing, default shader settings, no "
            "resolution scaling");
        diagnostic["phase"] = "translate";
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
        Shader::Maxwell::Flow::CFG cfg{env, flow_pool, Shader::Maxwell::Location{env.EntryPoint()}};
        Shader::ObjectPool<Shader::IR::Inst> inst_pool;
        Shader::ObjectPool<Shader::IR::Block> block_pool;
        const auto host = OfflineHost();
        auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host);
        if (!control.empty() && env.ShaderStage() == Stage::Compute) {
            if (program.info.uses_local_memory) {
                throw MissingContext("Archive control does not establish the local memory size");
            }
            for (const auto* block : program.blocks) {
                for (const auto& instruction : block->Instructions()) {
                    const auto name = Shader::IR::NameOf(instruction.GetOpcode());
                    if (name.starts_with("LoadShared") || name.starts_with("WriteShared") ||
                        name.starts_with("SharedAtomic")) {
                        throw MissingContext("Archive control does not establish the shared memory size");
                    }
                }
            }
        }
        Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
        diagnostic["phase"] = "spirv";
        const auto profile = OfflineProfile(host);
        Shader::Backend::Bindings bindings{};
        const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        diagnostic["spirv_words"] = spirv.size();
        diagnostic["phase"] = "output";
        WriteFile(output, spirv);
        diagnostic["status"] = "ok";
        std::cout << diagnostic.dump() << '\n';
        return 0;
    } catch (const MissingContext& error) {
        diagnostic["phase"] = "context";
        diagnostic["error"] = {{"code", "missing_context"}, {"message", error.what()}};
    } catch (const std::exception& error) {
        diagnostic["error"] = {
            {"code", diagnostic["phase"] == "input" ? "invalid_input" : "compilation_failed"},
            {"message", error.what()}};
    }
    std::cout << diagnostic.dump() << '\n';
    return diagnostic["phase"] == "input" ? 2 : 1;
}
