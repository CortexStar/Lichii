// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <future>
#include <iostream>
#include <stdexcept>

#include "core/file_sys/vfs/vfs_real.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "video_core/nup_shader_cache.h"

int main(int argc, char** argv) try {
    if (argc != 2) {
        return 2;
    }
    FileSys::RealVfsFilesystem filesystem;
    const auto file = filesystem.OpenFile(argv[1], FileSys::OpenMode::Read);
    VideoCommon::NupShaderCache cache;
    if (!cache.Prepare(file, std::filesystem::u8path(argv[1]), {}, [](size_t, size_t) {})) {
        throw std::runtime_error("preparation failed");
    }
    if (!cache.Identity()) throw std::runtime_error("prepared NUP lost its identity");
    std::vector<std::vector<u8>> controls;
    cache.ForEachComputeVariant({}, [&](std::span<const u8> source, std::span<const u8> control,
                                        const Shader::Ampere::LoweredProgram* lowered) {
        if (source.empty() || !lowered) throw std::runtime_error("variant lost its code");
        controls.emplace_back(control.begin(), control.end());
    });
    if (controls.size() != 2 || controls[0] == controls[1]) {
        throw std::runtime_error("lowering deduplication discarded a compute control variant");
    }
    size_t graphics_groups = 0, paired_groups = 0;
    const bool graphics_complete = cache.ForEachGraphicsGroup({}, [&](std::span<const Shader::Ampere::ArchiveProgram> group) {
        ++graphics_groups;
        if (group.size() == 2) ++paired_groups;
    });
    if (!graphics_complete || graphics_groups != 3 || paired_groups != 1) {
        throw std::runtime_error("graphics group linkage was lost or invented for BAHS records");
    }
    const std::array<Shader::Ampere::Instruction, 1> native{{{{0x794d, 0, 0x03800000, 0}}}};
    const auto verify = [&] {
        size_t nops = 0;
        for (const auto stage : {Shader::Stage::VertexB, Shader::Stage::Fragment,
                                Shader::Stage::Compute, Shader::Stage::Task, Shader::Stage::Mesh}) {
            std::vector<Shader::Ampere::Instruction> source(nops, {{0x7918, 0, 0, 0}});
            source.push_back(native[0]);
            const auto result = cache.Find(source, stage, false);
            if (!cache.HasProgram(source, stage)) throw std::runtime_error("cached program index was lost");
            if (!result || result->instructions.size() != source.size() ||
                result->instructions.back() != 0xe30000000007000fULL ||
                result->source_size != source.size() * 16 ||
                result->source_instruction_offsets.size() != source.size()) {
                std::cerr << "stage=" << static_cast<u32>(stage) << " found=" << bool(result);
                if (result) {
                    std::cerr << " words=" << result->instructions.size()
                              << " source=" << result->source_size
                              << " offsets=" << result->source_instruction_offsets.size();
                }
                std::cerr << '\n';
                throw std::runtime_error("offset lookup returned wrong shader");
            }
            ++nops;
        }
    };
    std::array<std::future<void>, 8> readers;
    for (auto& reader : readers) {
        reader = std::async(std::launch::async, verify);
    }
    for (auto& reader : readers) {
        reader.get();
    }
    auto changed = native;
    changed[0].words[1] = 1;
    if (cache.Find(changed, Shader::Stage::VertexB, false) ||
        cache.Find(native, Shader::Stage::Geometry, true)) {
        throw std::runtime_error("cache accepted a different program or stage");
    }
    std::stop_source stop;
    stop.request_stop();
    if (cache.Prepare(file, std::filesystem::u8path(argv[1]), stop.get_token(), [](size_t, size_t) {})) {
        throw std::runtime_error("cache ignored cancellation");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
