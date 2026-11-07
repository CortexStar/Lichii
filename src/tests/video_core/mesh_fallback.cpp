// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;
namespace SPIRV = Shader::Backend::SPIRV;

constexpr u32 OUTPUT_BINDING = 4;
constexpr u32 PAYLOAD_BINDING = 5;
constexpr u32 OFFSETS_BINDING = 6;
constexpr u32 ARGS_BINDING = 7;

struct Instruction {
    spv::Op op;
    std::span<const u32> operands;
};

std::vector<Instruction> Instructions(const std::vector<u32>& code) {
    std::vector<Instruction> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count{code[offset] >> 16};
        REQUIRE(count != 0);
        REQUIRE(offset + count <= code.size());
        result.push_back({static_cast<spv::Op>(code[offset] & 0xffff),
                          std::span<const u32>(code).subspan(offset + 1, count - 1)});
        offset += count;
    }
    return result;
}

spv::ExecutionModel Model(const std::vector<u32>& code) {
    for (const Instruction& inst : Instructions(code)) {
        if (inst.op == spv::Op::OpEntryPoint) {
            return static_cast<spv::ExecutionModel>(inst.operands[0]);
        }
    }
    FAIL("no entry point");
    return {};
}

bool HasCapability(const std::vector<u32>& code, spv::Capability capability) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return inst.op == spv::Op::OpCapability &&
               inst.operands[0] == static_cast<u32>(capability);
    });
}

bool HasExtension(const std::vector<u32>& code, std::string_view name) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return inst.op == spv::Op::OpExtension &&
               std::string_view{reinterpret_cast<const char*>(inst.operands.data())} == name;
    });
}

bool HasVariable(const std::vector<u32>& code, spv::StorageClass storage_class) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return inst.op == spv::Op::OpVariable &&
               inst.operands[2] == static_cast<u32>(storage_class);
    });
}

bool UsesStorageClass(const std::vector<u32>& code, spv::StorageClass storage_class) {
    return std::ranges::any_of(Instructions(code), [&](const Instruction& inst) {
        return (inst.op == spv::Op::OpTypePointer &&
                inst.operands[1] == static_cast<u32>(storage_class)) ||
               (inst.op == spv::Op::OpVariable &&
                inst.operands[2] == static_cast<u32>(storage_class));
    });
}

std::vector<u32> Bindings(const std::vector<u32>& code) {
    std::vector<u32> bindings;
    for (const Instruction& inst : Instructions(code)) {
        if (inst.op == spv::Op::OpDecorate &&
            inst.operands[1] == static_cast<u32>(spv::Decoration::Binding)) {
            bindings.push_back(inst.operands[2]);
        }
    }
    std::ranges::sort(bindings);
    return bindings;
}

// EDEN_MESH_FALLBACK_TEST_OUTPUT=<dir>: write the modules for spirv-val.
void Dump(const std::vector<u32>& code, std::string_view name) {
    const char* const directory{std::getenv("EDEN_MESH_FALLBACK_TEST_OUTPUT")};
    if (!directory) {
        return;
    }
    std::filesystem::create_directories(directory);
    std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                       std::ios::binary};
    file.write(reinterpret_cast<const char*>(code.data()),
               static_cast<std::streamsize>(code.size() * sizeof(u32)));
    REQUIRE(file.good());
}

MeshInfo TriangleMesh() {
    MeshInfo mesh;
    mesh.payload_words = 32;
    mesh.max_vertices = 3;
    mesh.max_primitives = 1;
    mesh.topology = 4;
    for (const IR::Attribute attribute : {IR::Attribute::PositionX, IR::Attribute::PositionY,
                                          IR::Attribute::PositionZ, IR::Attribute::PositionW}) {
        mesh.vertex_lines.push_back(static_cast<u32>(attribute));
    }
    return mesh;
}

Profile MakeProfile() {
    Profile profile;
    profile.supported_spirv = 0x00010500;
    return profile;
}

// A one-triangle mesh program (or the task program launching it), as the frontend lowers them.
// With lane_id the mesh program writes its lane as the vertex's first attribute word.
std::vector<u32> EmitProgram(Stage stage, bool as_compute, bool with_task,
                             Profile profile = MakeProfile(), bool lane_id = false,
                             bool culled = false) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = stage;
    program.workgroup_size = {32, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.mesh = TriangleMesh();
    program.info.is_task = stage == Stage::Task;
    program.info.is_mesh = stage == Stage::Mesh;
    program.info.uses_workgroup_id = true;
    program.info.uses_local_invocation_id = true;
    IR::IREmitter ir{block};
    ir.Prologue();
    if (stage == Stage::Mesh) {
        const IR::U32 index{lane_id ? ir.LaneId() : ir.IsbeRead(ir.Imm32(0))};
        program.info.uses_subgroup_invocation_id = lane_id;
        ir.IsbeWriteAttribute(ir.Imm32(0), index);
        ir.IsbeWriteMapU8(ir.Imm32(4), ir.Imm32(0));
        ir.IsbeWriteMapU32(ir.Imm32(3), ir.Imm32(1));
    } else {
        if (!culled) {
            ir.IsbeWriteAttribute(ir.Imm32(4), ir.Imm32(1)); // one mesh workgroup
        }
    }
    ir.Epilogue();

    RuntimeInfo runtime_info;
    runtime_info.mesh_as_compute = as_compute;
    runtime_info.mesh_output_binding = OUTPUT_BINDING;
    if (with_task) {
        runtime_info.mesh_task_payload_binding = PAYLOAD_BINDING;
        runtime_info.mesh_task_offsets_binding = OFFSETS_BINDING;
        runtime_info.mesh_task_args_binding = ARGS_BINDING;
        runtime_info.mesh_task_payload_stride = program.mesh.payload_words;
    }
    Backend::Bindings bindings;
    return SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
}

void RequireNoMeshShading(const std::vector<u32>& code) {
    REQUIRE(Model(code) == spv::ExecutionModel::GLCompute);
    REQUIRE_FALSE(HasCapability(code, spv::Capability::MeshShadingEXT));
    REQUIRE_FALSE(HasExtension(code, "SPV_EXT_mesh_shader"));
    REQUIRE_FALSE(UsesStorageClass(code, spv::StorageClass::TaskPayloadWorkgroupEXT));
    REQUIRE_FALSE(HasVariable(code, spv::StorageClass::Output));
}
// Result-id tables for the store checks below.
struct ModuleData {
    std::vector<Instruction> ops;
    std::unordered_map<u32, u32> constants;                    // id -> value
    std::unordered_map<u32, spv::StorageClass> variables;      // id -> storage class
    std::unordered_map<u32, std::vector<u32>> chains;          // id -> [base, indexes...]
    std::unordered_map<u32, std::pair<u32, u32>> umins;        // id -> (operand a, operand b)
    std::unordered_map<u32, std::pair<u32, u32>> shifts;       // id -> (base, shift)
};

ModuleData Analyze(const std::vector<u32>& code) {
    ModuleData data;
    for (const Instruction& inst : Instructions(code)) {
        switch (inst.op) {
        case spv::Op::OpConstant:
            data.constants[inst.operands[1]] = inst.operands[2];
            break;
        case spv::Op::OpVariable:
            data.variables[inst.operands[1]] = static_cast<spv::StorageClass>(inst.operands[2]);
            break;
        case spv::Op::OpAccessChain:
        case spv::Op::OpInBoundsAccessChain:
            data.chains[inst.operands[1]] = {inst.operands.begin() + 2, inst.operands.end()};
            break;
        case spv::Op::OpShiftRightLogical:
            data.shifts[inst.operands[1]] = {inst.operands[2], inst.operands[3]};
            break;
        case spv::Op::OpExtInst:
            // The emitter lowers integer min/max to the GLSL.std.450 extended instruction set.
            if (inst.operands[3] == GLSLstd450UMin) {
                data.umins[inst.operands[1]] = {inst.operands[4], inst.operands[5]};
            }
            break;
        default:
            break;
        }
        data.ops.push_back(inst);
    }
    return data;
}

// Resolves an id to a constant value, folding OpUMin of constants (the payload pointer clamps
// its index that way) and OpShiftRightLogical (the guest's byte address to word index).
std::optional<u32> ConstValue(const ModuleData& data, u32 id) {
    if (const auto it{data.constants.find(id)}; it != data.constants.end()) {
        return it->second;
    }
    if (const auto it{data.umins.find(id)}; it != data.umins.end()) {
        const std::optional<u32> a{ConstValue(data, it->second.first)};
        const std::optional<u32> b{ConstValue(data, it->second.second)};
        if (a && b) {
            return (std::min)(*a, *b);
        }
    }
    if (const auto it{data.shifts.find(id)}; it != data.shifts.end()) {
        const std::optional<u32> a{ConstValue(data, it->second.first)};
        const std::optional<u32> b{ConstValue(data, it->second.second)};
        if (a && b) {
            return *a >> *b;
        }
    }
    return std::nullopt;
}

// The word index when a pointer is an access chain into the TaskPayloadWorkgroupEXT variable.
std::optional<u32> PayloadWordIndex(const ModuleData& data, u32 pointer) {
    const auto it{data.chains.find(pointer)};
    if (it == data.chains.end() || it->second.size() != 2) {
        return std::nullopt;
    }
    const auto var{data.variables.find(it->second[0])};
    if (var == data.variables.end() || var->second != spv::StorageClass::TaskPayloadWorkgroupEXT) {
        return std::nullopt;
    }
    return ConstValue(data, it->second[1]);
}

constexpr u32 WORKGROUP{static_cast<u32>(spv::Scope::Workgroup)};
constexpr u32 BARRIER_SEMANTICS{static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease |
                                                 spv::MemorySemanticsMask::WorkgroupMemory)};

// A fully checked OpControlBarrier: Workgroup execution and memory scope, AcquireRelease |
// WorkgroupMemory semantics.
bool IsPayloadBarrier(const ModuleData& data, const Instruction& inst) {
    return inst.op == spv::Op::OpControlBarrier &&
           ConstValue(data, inst.operands[0]) == std::optional<u32>{WORKGROUP} &&
           ConstValue(data, inst.operands[1]) == std::optional<u32>{WORKGROUP} &&
           ConstValue(data, inst.operands[2]) == std::optional<u32>{BARRIER_SEMANTICS};
}

} // Anonymous namespace

TEST_CASE("Mesh fallback leaves native task and mesh programs as mesh shaders",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> mesh{EmitProgram(Stage::Mesh, false, false)};
    Dump(mesh, "native-mesh");
    REQUIRE(Model(mesh) == spv::ExecutionModel::MeshEXT);
    REQUIRE(HasCapability(mesh, spv::Capability::MeshShadingEXT));
    REQUIRE(Bindings(mesh).empty());

    const std::vector<u32> task{EmitProgram(Stage::Task, false, false)};
    Dump(task, "native-task");
    REQUIRE(Model(task) == spv::ExecutionModel::TaskEXT);
    REQUIRE(UsesStorageClass(task, spv::StorageClass::TaskPayloadWorkgroupEXT));
}

TEST_CASE("Mesh fallback native task initializes the mesh workgroup count atomically",
          "[video_core][spirv][mesh_fallback]") {
    // A task that culls every thread never writes the mesh workgroup count, and the payload
    // starts undefined. Intel's garbage there launches enough workgroups to lose the device, so
    // the prologue initializes the count to zero. Every invocation writes the same word, hence
    // an atomic store: concurrent plain stores race even when they write the same value.
    const std::vector<u32> culled{EmitProgram(Stage::Task, false, false, MakeProfile(), false,
                                              true)};
    Dump(culled, "native-task-culled");
    REQUIRE(Model(culled) == spv::ExecutionModel::TaskEXT);
    const ModuleData data{Analyze(culled)};
    size_t atomic_inits{0};
    size_t atomic_position{data.ops.size()};
    size_t first_barrier{data.ops.size()};
    size_t guest_stores{0};
    for (size_t i = 0; i < data.ops.size(); ++i) {
        const Instruction& inst{data.ops[i]};
        // OpAtomicStore has no result: pointer, scope, semantics, value.
        if (inst.op == spv::Op::OpAtomicStore &&
            PayloadWordIndex(data, inst.operands[0]) == std::optional<u32>{1}) {
            ++atomic_inits;
            atomic_position = i;
            // Workgroup scope, relaxed semantics: the paired control barriers do the ordering.
            REQUIRE(ConstValue(data, inst.operands[1]) == std::optional<u32>{WORKGROUP});
            REQUIRE(ConstValue(data, inst.operands[2]) == std::optional<u32>{0});
            // The initialized value is zero.
            REQUIRE(ConstValue(data, inst.operands[3]) == std::optional<u32>{0});
        }
        if (IsPayloadBarrier(data, inst) && first_barrier == data.ops.size()) {
            first_barrier = i;
        }
        // OpStore: pointer, object.
        if (inst.op == spv::Op::OpStore &&
            PayloadWordIndex(data, inst.operands[0]) == std::optional<u32>{1}) {
            ++guest_stores;
        }
    }
    REQUIRE(atomic_inits == 1);
    // A culled task never writes the count itself; the atomic init is the only write.
    REQUIRE(guest_stores == 0);
    // The init runs in the prologue, before the barrier that orders it against guest writes.
    REQUIRE(first_barrier < data.ops.size());
    REQUIRE(atomic_position < first_barrier);
}

TEST_CASE("Mesh fallback native task keeps the guest's count path after the atomic init",
          "[video_core][spirv][mesh_fallback]") {
    // A task that launches meshes writes the count itself after the prologue, and the epilogue
    // reads word 1 back for OpEmitMeshTasksEXT. Both must survive the init.
    const std::vector<u32> task{EmitProgram(Stage::Task, false, false)};
    Dump(task, "native-task-count");
    const ModuleData data{Analyze(task)};
    size_t atomic_inits{0};
    size_t atomic_position{0};
    size_t guest_stores{0};
    size_t guest_position{0};
    size_t count_reads{0};
    size_t read_position{0};
    u32 loaded_count{0};
    size_t emitted{0};
    std::vector<size_t> barriers;
    for (size_t i = 0; i < data.ops.size(); ++i) {
        const Instruction& inst{data.ops[i]};
        if (inst.op == spv::Op::OpAtomicStore &&
            PayloadWordIndex(data, inst.operands[0]) == std::optional<u32>{1}) {
            ++atomic_inits;
            atomic_position = i;
            REQUIRE(ConstValue(data, inst.operands[1]) == std::optional<u32>{WORKGROUP});
            REQUIRE(ConstValue(data, inst.operands[2]) == std::optional<u32>{0});
            REQUIRE(ConstValue(data, inst.operands[3]) == std::optional<u32>{0});
        }
        // The guest's own count write must be the only plain store to word 1, storing 1.
        if (inst.op == spv::Op::OpStore &&
            PayloadWordIndex(data, inst.operands[0]) == std::optional<u32>{1}) {
            ++guest_stores;
            guest_position = i;
            REQUIRE(ConstValue(data, inst.operands[1]) == std::optional<u32>{1});
        }
        // OpLoad: result type, result id, pointer.
        if (inst.op == spv::Op::OpLoad &&
            PayloadWordIndex(data, inst.operands[2]) == std::optional<u32>{1}) {
            ++count_reads;
            read_position = i;
            loaded_count = inst.operands[1];
        }
        // The loaded count must feed OpEmitMeshTasksEXT as GroupCountX.
        if (inst.op == spv::Op::OpEmitMeshTasksEXT) {
            REQUIRE(inst.operands[0] == loaded_count);
            REQUIRE(count_reads >= 1);
            ++emitted;
        }
        if (IsPayloadBarrier(data, inst)) {
            barriers.push_back(i);
        }
    }
    REQUIRE(atomic_inits == 1);
    REQUIRE(guest_stores == 1);
    REQUIRE(count_reads == 1);
    REQUIRE(emitted == 1);
    // init, prologue barrier, guest write, epilogue barrier, count read.
    REQUIRE(barriers.size() >= 2);
    REQUIRE(atomic_position < barriers.front());
    REQUIRE(barriers.front() < guest_position);
    REQUIRE(guest_position < barriers.back());
    REQUIRE(barriers.back() < read_position);

    // The as-compute path zeroes the whole payload region in its prologue instead, with
    // per-word strided plain stores, so it needs no atomic.
    const std::vector<u32> compute{EmitProgram(Stage::Task, true, true)};
    Dump(compute, "fallback-task-count");
    const ModuleData compute_data{Analyze(compute)};
    const size_t compute_atomics{static_cast<size_t>(
        std::ranges::count_if(compute_data.ops, [](const Instruction& inst) {
            return inst.op == spv::Op::OpAtomicStore;
        }))};
    REQUIRE(compute_atomics == 0);
}

TEST_CASE("Mesh fallback emits mesh programs as compute shaders writing storage buffers",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> alone{EmitProgram(Stage::Mesh, true, false)};
    Dump(alone, "fallback-mesh");
    RequireNoMeshShading(alone);
    REQUIRE(Bindings(alone) == std::vector<u32>{OUTPUT_BINDING});

    const std::vector<u32> after_task{EmitProgram(Stage::Mesh, true, true)};
    Dump(after_task, "fallback-mesh-after-task");
    RequireNoMeshShading(after_task);
    REQUIRE(Bindings(after_task) ==
            std::vector<u32>{OUTPUT_BINDING, PAYLOAD_BINDING, OFFSETS_BINDING, ARGS_BINDING});
}

TEST_CASE("Mesh fallback programs use compute subgroup support",
          "[video_core][spirv][mesh_fallback]") {
    // A device without VK_EXT_mesh_shader reports subgroup support for compute, not mesh.
    Profile profile{MakeProfile()};
    profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Compute);
    const std::vector<u32> code{EmitProgram(Stage::Mesh, true, false, profile, true)};
    Dump(code, "fallback-mesh-lane-id");
    RequireNoMeshShading(code);
    REQUIRE(HasCapability(code, spv::Capability::GroupNonUniform));
}

TEST_CASE("Mesh fallback emits task programs as compute shaders writing their payload",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> task{EmitProgram(Stage::Task, true, true)};
    Dump(task, "fallback-task");
    RequireNoMeshShading(task);
    REQUIRE(Bindings(task) == std::vector<u32>{PAYLOAD_BINDING});
}

TEST_CASE("Mesh fallback vertex shader pulls the compute output",
          "[video_core][spirv][mesh_fallback]") {
    const MeshInfo mesh{TriangleMesh()};
    const std::vector<u32> alone{
        SPIRV::EmitMeshEmulationVertex(MakeProfile(), mesh, OUTPUT_BINDING, ~0U, false)};
    Dump(alone, "fallback-vertex");
    REQUIRE(Model(alone) == spv::ExecutionModel::Vertex);
    REQUIRE(Bindings(alone) == std::vector<u32>{OUTPUT_BINDING});

    const std::vector<u32> after_task{
        SPIRV::EmitMeshEmulationVertex(MakeProfile(), mesh, OUTPUT_BINDING, ARGS_BINDING, true)};
    Dump(after_task, "fallback-vertex-after-task");
    REQUIRE(Model(after_task) == spv::ExecutionModel::Vertex);
    REQUIRE(Bindings(after_task) == std::vector<u32>{OUTPUT_BINDING, ARGS_BINDING});
}

TEST_CASE("Mesh fallback distribution pass binds its buffers and constants",
          "[video_core][spirv][mesh_fallback]") {
    const std::vector<u32> code{SPIRV::EmitMeshEmulationDistribution(MakeProfile())};
    Dump(code, "fallback-distribution");
    REQUIRE(Model(code) == spv::ExecutionModel::GLCompute);
    REQUIRE(Bindings(code) == std::vector<u32>{0, 1, 2, 3});
    REQUIRE(UsesStorageClass(code, spv::StorageClass::PushConstant));
    const auto instructions{Instructions(code)};
    const bool has_constants{std::ranges::any_of(instructions, [](const Instruction& inst) {
        return inst.op == spv::Op::OpTypeStruct &&
               inst.operands.size() == 1 + sizeof(SPIRV::MeshEmulationDistributionConstants) / 4;
    })};
    REQUIRE(has_constants);
}
