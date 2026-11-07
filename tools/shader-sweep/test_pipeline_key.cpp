// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Key {
    std::array<u64, 6> shaders;
    Vulkan::FixedPipelineState state;
};
static_assert(sizeof(Key) == 1056);

std::string Bytes(const Key& key) {
    return {reinterpret_cast<const char*>(&key), sizeof(key.shaders) + key.state.Size()};
}
} // namespace

int main(int argc, char** argv) try {
    Vulkan::FixedPipelineState first{};
    first.dynamic_vertex_input.Assign(1);
    first.extended_dynamic_state_3_blend.Assign(1);
    first.attribute_types = 0x5010015;
    first.color_formats[0] = 0xe0;
    auto second = first;
    second.attribute_types = 0x10013115;
    first.ClearMeshVertexInput();
    second.ClearMeshVertexInput();
    Require(std::memcmp(&first, &second, sizeof(first)) == 0,
            "Unused mesh vertex types still split identical states");
    Require(first.color_formats[0] == 0xe0, "Mesh normalization changed render target state");
    second.color_formats[0] = 0xd5;
    Require(std::memcmp(&first, &second, sizeof(first)) != 0,
            "Mesh normalization merged different render targets");
    auto repeated = first;
    repeated.ClearMeshVertexInput();
    Require(std::memcmp(&first, &repeated, sizeof(first)) == 0, "Normalization is not idempotent");
    Vulkan::FixedPipelineState static_input{};
    static_input.enabled_divisors = 0x12345;
    const auto unchanged = static_input;
    static_input.ClearMeshVertexInput();
    Require(std::memcmp(&static_input, &unchanged, sizeof(static_input)) == 0,
            "Static vertex divisor state was overwritten");
    std::cout << "Mesh pipeline key normalization tests passed\n";

    if (argc == 3) {
        std::set<std::string> before, after;
        size_t records = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1])) {
            if (entry.path().filename() != "key.bin") continue;
            Require(std::filesystem::exists(entry.path().parent_path() / "mesh.env"),
                    "Supplied key has no mesh environment");
            Require(std::filesystem::file_size(entry.path()) == sizeof(Key), "Unexpected key size");
            Key key;
            std::ifstream stream(entry.path(), std::ios::binary);
            Require(bool(stream.read(reinterpret_cast<char*>(&key), sizeof(key))), "Cannot read key");
            before.insert(Bytes(key));
            key.state.ClearMeshVertexInput();
            after.insert(Bytes(key));
            ++records;
        }
        Require(records > 0 && after.size() == std::stoull(argv[2]), "Unexpected normalized key count");
        std::cout << records << " captured mesh keys: " << before.size() << " -> " << after.size() << '\n';
    } else if (argc != 1) return 2;
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
