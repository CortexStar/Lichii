// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "shader_recompiler/frontend/ampere/shader_archive.h"

namespace {
void Require(bool condition) {
    if (!condition) {
        throw std::runtime_error("archive regression failed");
    }
}

template <typename T>
void Put(std::vector<u8>& data, size_t offset, T value) {
    std::memcpy(data.data() + offset, &value, sizeof(value));
}

std::vector<u8> Fixture(u32 stage = 0) {
    std::vector<u8> data(0x400);
    std::memcpy(data.data(), "BNSH\0\0\0\0", 8);
    Put<u32>(data, 8, 0x00020400);
    Put<u16>(data, 12, 0xfeff);
    Put<u32>(data, 0x1c, static_cast<u32>(data.size()));
    Put<u32>(data, 0x60, 0x63737267);
    Put<u16>(data, 0x70, 4);
    Put<u16>(data, 0x72, 200);
    Put<u32>(data, 0x7c, 1);
    Put<u64>(data, 0x80, 0xc0);
    Put<u64>(data, 0xd0, 0x100);
    Put<u64>(data, 0x108 + stage * 8, 0x140);
    Put<u64>(data, 0x148, 0x180);
    Put<u64>(data, 0x150, 0x200);
    Put<u32>(data, 0x158, 0x100);
    Put<u32>(data, 0x15c, 4);
    Put<u32>(data, 0x200, (stage + 1) << 10);
    return data;
}

std::vector<u8> SharcbFixture(u32 slot) {
    std::vector<u8> data(0x800);
    std::memcpy(data.data(), "BAHS", 4);
    Put<u32>(data, 4, 11);
    Put<u32>(data, 8, data.size());
    Put<u32>(data, 12, 5);
    Put<u32>(data, 20, 0x200);
    Put<u32>(data, 24, 0x200);
    Put<u32>(data, 28, 0x600);
    Put<u32>(data, 0x350, 0x188);
    Put<u32>(data, 0x354, 2);
    for (const size_t record : {size_t{0x358}, size_t{0x458}}) {
        const u32 length = record == 0x358 ? 0x100 : 0x80;
        Put<u32>(data, record, length);
        Put<u32>(data, record + 4, slot);
        Put<u32>(data, record + 12, length - 16);
        Put<u32>(data, record + 24, 104);
        Put<s32>(data, record + 28, static_cast<s32>(0x3a8 - (record + 16)));
    }
    Put<u64>(data, 0x3a8, 0x000000c998761234ULL);
    Put<u32>(data, 0x3e0, slot == 3 ? 16 : 144);
    Put<u32>(data, 0x3e8, 256);
    Put<u32>(data, 0x3ec, 512);
    Put<u32>(data, 0x3fc, (slot == 0 ? 0 : slot == 1 ? 1 : slot + 2) << 16);
    Put<u32>(data, 0x600, (slot == 1 ? 5 : 1) << 10);
    Put<u32>(data, 0x700, 0x12345678);
    return data;
}
} // namespace

int main() try {
    using namespace Shader::Ampere;
    for (u32 stage = 0; stage < 6; ++stage) {
        const auto data = Fixture(stage);
        size_t count = 0;
        Require(ReadBnsh(data, [&](const ArchiveProgram& program) {
            Require(static_cast<u32>(program.stage) == stage);
            Require(program.code.size() == 0x100 && program.control.size() == 4);
            Require(program.variation == 0 && program.container.data() == data.data());
            ++count;
            return true;
        }));
        Require(count == 1);
        Require(!ReadBnsh(data, [](const ArchiveProgram&) { return false; }));
    }
    for (const size_t offset : {size_t{0x7c}, size_t{0x150}, size_t{0x158}, size_t{0x200}}) {
        auto data = Fixture();
        Put<u32>(data, offset, 0xffffffff);
        bool rejected = false;
        try {
            ReadBnsh(data, [](const ArchiveProgram&) { return true; });
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Require(rejected);
    }
    auto embedded = Fixture();
    embedded.insert(embedded.begin(), 16, 0);
    const auto second = Fixture(5);
    embedded.insert(embedded.end(), second.begin(), second.end());
    size_t count = 0;
    Require(ReadEmbeddedBnsh(embedded, [&](const ArchiveProgram&) { ++count; return true; }));
    Require(count == 2);
    embedded.pop_back();
    bool rejected = false;
    try {
        ReadEmbeddedBnsh(embedded, [](const ArchiveProgram&) { return true; });
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Require(rejected);
    for (const u32 slot : {0U, 1U, 3U, 4U, 5U}) {
        const auto sharcb = SharcbFixture(slot);
        size_t records = 0;
        Require(ReadShaderArchive(sharcb, [&](const ArchiveProgram& program) {
            Require(!program.variation && program.container.data() == sharcb.data());
            Require(program.code.size() == 512 && program.control.size() == 104);
            Require(program.code[256] == 0x78);
            Require(program.stage == (slot == 0 ? Shader::Stage::VertexB : slot == 1 ?
                Shader::Stage::Fragment : slot == 3 ? Shader::Stage::Compute : slot == 4 ?
                Shader::Stage::Task : Shader::Stage::Mesh));
            ++records;
            return true;
        }));
        Require(records == 2);
        Require(!ReadShaderArchive(sharcb, [](const ArchiveProgram&) { return false; }));
    }
    for (const auto [offset, value] : {std::pair<size_t, u32>{8, 0x801},
            {0x358, 0}, {0x368, 1}, {0x474, 0x80000000}, {0x3e0, 0}, {0x3fc, 5U << 16}}) {
        auto sharcb = SharcbFixture(0);
        Put<u32>(sharcb, offset, value);
        rejected = false;
        try { ReadShaderArchive(sharcb, [](const ArchiveProgram&) { return true; }); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected);
    }
    const auto sharcb = SharcbFixture(3);
    std::vector<u8> sarc(0x100 + sharcb.size());
    std::memcpy(sarc.data(), "SARC", 4);
    Put<u16>(sarc, 4, 20); Put<u16>(sarc, 6, 0xfeff);
    Put<u32>(sarc, 8, sarc.size()); Put<u32>(sarc, 12, 0x100);
    std::memcpy(sarc.data() + 20, "SFAT", 4);
    Put<u16>(sarc, 24, 12); Put<u16>(sarc, 26, 1);
    Put<u32>(sarc, 44, sharcb.size());
    std::copy(sharcb.begin(), sharcb.end(), sarc.begin() + 0x100);
    size_t nested_count = 0;
    Require(ReadShaderArchive(sarc, [&](const ArchiveProgram&) { ++nested_count; return true; }));
    Require(nested_count == 2);
    Require(!ReadShaderArchive(sarc, [](const ArchiveProgram&) { return false; }));
    Put<u32>(sarc, 44, sharcb.size() + 1);
    rejected = false;
    try { ReadShaderArchive(sarc, [](const ArchiveProgram&) { return true; }); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected);
    auto mesh = SharcbFixture(5);
    Put<u32>(mesh, 0x600, 3U << 10);
    Require(ReadShaderArchive(mesh, [](const ArchiveProgram&) { return true; }));
    auto task = SharcbFixture(4);
    Put<u32>(task, 0x600, 3U << 10);
    rejected = false;
    try { ReadShaderArchive(task, [](const ArchiveProgram&) { return true; }); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected);
    std::vector<u8> control(224);
    Put<u64>(control, 0, 0x000000c998761234ULL);
    Put<u32>(control, 84, 5U << 16);
    control[0xa9] = 5;
    control[0xb1] = 2;
    control[0xb2] = 1;
    control[0xb3] = 1;
    Put<u16>(control, 0xba, 104);
    const auto layout = ReadDeclaredComputeBindings(control);
    Require(layout && layout->first_pointer == 48 && layout->pointer_count == 3);
    Put<u16>(control, 0xba, 103);
    rejected = false;
    try {
        ReadDeclaredComputeBindings(control);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Require(rejected);
    std::vector<u8> code(20);
    Put<u32>(control, 0x38, 16);
    Put<u32>(control, 0x40, 16);
    Put<u32>(control, 0x44, 20);
    const auto sections = ReadArchiveCodeSections(code, control);
    Require(sections && sections->instruction_bytes == 16 && sections->constants.size() == 4);
    code.resize(16);
    Put<u32>(control, 0x40, 256);
    Put<u32>(control, 0x44, 256);
    Require(ReadArchiveCodeSections(code, control)->constants.empty());
    Put<u32>(control, 0x44, 260);
    rejected = false;
    try { ReadArchiveCodeSections(code, control); } catch (const std::exception&) { rejected = true; }
    Require(rejected);
    std::vector<u8> mesh_control(224), mesh_code(512);
    Put<u64>(mesh_control, 0, 0x000000c998761234ULL);
    Put<u32>(mesh_control, 84, 7U << 16);
    Put<u16>(mesh_control, 0x58, 32);
    Put<u16>(mesh_control, 0x5a, 3200);
    mesh_control[0x5e] = 1;
    Put<u16>(mesh_control, 0x60, 128);
    Put<u16>(mesh_control, 0x62, 64);
    mesh_control[0x68] = 0x80;
    Put<u32>(mesh_control, 0x64, 384);
    Put<u32>(mesh_code, 384, 4U << 10);
    auto mesh_info = ReadDeclaredMeshInfo(mesh_code, mesh_control);
    Require(mesh_info && mesh_info->threads == 32 && mesh_info->payload_bytes == 3200 &&
            mesh_info->max_vertices == 128 && mesh_info->max_primitives == 64 &&
            mesh_info->has_task && mesh_info->primitive_header.size() == 128);
    Put<u32>(mesh_control, 0x64, 385);
    rejected = false;
    try { ReadDeclaredMeshInfo(mesh_code, mesh_control); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected);
    Put<u32>(mesh_control, 84, 6U << 16);
    mesh_info = ReadDeclaredMeshInfo(mesh_code, mesh_control);
    Require(mesh_info && mesh_info->threads == 32 && mesh_info->payload_bytes == 3200 &&
            mesh_info->max_vertices == 0 && !mesh_info->has_task && mesh_info->primitive_header.empty());
    Put<u16>(mesh_control, 0x58, 0);
    rejected = false;
    try { ReadDeclaredMeshInfo(mesh_code, mesh_control); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected);
    std::cout << "NVN2 archive bounds, stages, embedded programs and cancellation passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
