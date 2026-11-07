// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/file_sys/vfs/vfs_real.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "video_core/nup_shader_cache.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<u8> ReadBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.is_open(), "Cannot read fixture file");
    return {std::istreambuf_iterator<char>{file}, {}};
}

void CheckNativeLookup(const VideoCommon::NupShaderCache& cache) {
    size_t nops = 0;
    for (const auto stage : {Shader::Stage::VertexB, Shader::Stage::Fragment,
                            Shader::Stage::Compute, Shader::Stage::Task, Shader::Stage::Mesh}) {
        std::vector<Shader::Ampere::Instruction> code(nops++, {{0x7918, 0, 0, 0}});
        code.push_back({{0x794d, 0, 0x03800000, 0}});
        const auto lowered = cache.Find(code, stage, false);
        Require(cache.HasProgram(code, stage) && lowered &&
                    lowered->instructions.size() == code.size() &&
                    lowered->source_size == code.size() * sizeof(code[0]) &&
                    lowered->instructions.back() == 0xe30000000007000fULL,
                "Embedded driver cache damaged native shader lookup");
    }
}

void CheckBlob(const VideoCommon::NupShaderCache& cache, const std::array<u8, 32>& profile,
               const std::vector<u8>& expected) {
    const auto result = cache.ReadDriverCache(profile);
    Require(result && *result == expected, "Embedded driver blob differs from written bytes");
}

} // namespace

int main(int argc, char** argv) try {
    if (argc != 2) return 2;
    const auto source = std::filesystem::u8path(argv[1]);
    const auto original_source = ReadBytes(source);
    Require(original_source.size() < 1024 * 1024, "Expected the small synthetic NUP fixture");
    auto path = source;
    path.replace_extension(".shaders.bin");
    FileSys::RealVfsFilesystem filesystem;
    const auto file = filesystem.OpenFile(source.string(), FileSys::OpenMode::Read);
    const auto prepare = [&](VideoCommon::NupShaderCache& cache) {
        Require(cache.Prepare(file, source, {}, [](size_t, size_t) {}), "Fixture preparation failed");
        CheckNativeLookup(cache);
    };

    std::array<u8, 32> profile{}, wrong_profile{};
    for (size_t index = 0; index < profile.size(); ++index) {
        profile[index] = static_cast<u8>(index + 1);
    }
    wrong_profile = profile;
    wrong_profile.back() ^= 1;
    std::vector<u8> first(65539), second(8197);
    for (size_t index = 0; index < first.size(); ++index) first[index] = static_cast<u8>(index * 13);
    for (size_t index = 0; index < second.size(); ++index) second[index] = static_cast<u8>((index * 7) ^ 0xa5);

    VideoCommon::NupShaderCache cache;
    prepare(cache);
    const auto identity = cache.Identity();
    Require(!cache.ReadDriverCache(profile), "Fresh fixture unexpectedly contains a driver blob");
    Require(cache.WriteDriverCache(profile, first), "Initial driver blob write failed");
    CheckBlob(cache, profile, first);
    const auto before_repeat = ReadBytes(path);
    const auto before_repeat_time = std::filesystem::last_write_time(path);
    Require(cache.WriteDriverCache(profile, first), "Identical driver blob write failed");
    Require(ReadBytes(path) == before_repeat && std::filesystem::last_write_time(path) == before_repeat_time,
            "Identical verified driver blob was rewritten");
    Require(!cache.ReadDriverCache(wrong_profile), "Wrong driver profile was accepted");
    CheckNativeLookup(cache);
    Require(cache.Identity() == identity, "Driver write changed native cache identity");

    VideoCommon::NupShaderCache reopened;
    const auto before_reopen = ReadBytes(path);
    prepare(reopened);
    Require(ReadBytes(path) == before_reopen, "Reopening rewrote the embedded cache");
    CheckBlob(reopened, profile, first);
    Require(!reopened.ReadDriverCache(wrong_profile), "Reopening lost the profile guard");
    Require(reopened.WriteDriverCache(profile, second), "Replacement driver blob write failed");
    CheckBlob(reopened, profile, second);
    CheckNativeLookup(reopened);
    const auto after_replacement = ReadBytes(path);
    Require(!cache.WriteDriverCache(profile, first), "Stale writer replaced a newer cache index");
    Require(ReadBytes(path) == after_replacement, "Stale writer changed the cache file");

    const auto before_cancel = ReadBytes(path);
    const auto before_cancel_time = std::filesystem::last_write_time(path);
    std::stop_source cancelled;
    cancelled.request_stop();
    Require(!reopened.WriteDriverCache(wrong_profile, first, cancelled.get_token()),
            "Cancelled driver blob write succeeded");
    Require(ReadBytes(path) == before_cancel &&
                std::filesystem::last_write_time(path) == before_cancel_time,
            "Cancelled write changed the cache file");
    CheckBlob(reopened, profile, second);
    Require(!reopened.ReadDriverCache(wrong_profile), "Cancelled write changed the profile");

    VideoCommon::NupShaderCache final_cache;
    prepare(final_cache);
    CheckBlob(final_cache, profile, second);
    Require(final_cache.Identity() == identity, "Replacement changed native cache identity");

    // Corrupt only the compressed driver record, leaving the checked index and native records intact.
    const auto bytes = ReadBytes(path);
    Require(bytes.size() >= 56, "Missing indexed cache header");
    u64 index_offset{}, index_size{};
    std::memcpy(&index_offset, bytes.data() + 8, sizeof(index_offset));
    std::memcpy(&index_size, bytes.data() + 16, sizeof(index_size));
    Require(index_offset >= 56 && index_offset <= bytes.size() &&
                index_size == bytes.size() - index_offset, "Invalid cache index bounds");
    const auto index = nlohmann::json::from_cbor(bytes.begin() + index_offset, bytes.end());
    const auto& driver = index.at("driver");
    const u64 offset = driver.at("offset");
    const u64 size = driver.at("size");
    Require(offset >= 56 && offset < index_offset && size != 0 && size <= index_offset - offset,
            "Invalid driver record bounds");
    Require(driver.at("raw_size").get<size_t>() == second.size(), "Wrong uncompressed driver size");
    Require(driver.contains("checksum") && driver.contains("profile"), "Driver record lacks guards");
    const auto corrupt_at = offset + size / 2;
    {
        std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.seekp(static_cast<std::streamoff>(corrupt_at));
        const char corrupt = static_cast<char>(bytes[corrupt_at] ^ 1);
        output.write(&corrupt, 1);
        output.close();
    }
    Require(!final_cache.ReadDriverCache(profile), "Corrupted driver record was accepted");
    CheckNativeLookup(final_cache);
    VideoCommon::NupShaderCache corrupt_reopen;
    prepare(corrupt_reopen);
    Require(!corrupt_reopen.ReadDriverCache(profile), "Corrupted driver record survived reopening");
    Require(ReadBytes(source) == original_source, "Test modified the source NUP");
    std::cout << "Embedded NUP driver cache tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
