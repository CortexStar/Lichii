// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "common/scope_exit.h"
#include "common/uuid.h"
#include "video_core/renderer_vulkan/pipeline_cache_certificate.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void WriteBytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(bytes.data(), bytes.size());
    file.close();
}

} // namespace

int main() try {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() /
                           ("lichii-certificate-test-" + Common::UUID::MakeRandom().RawString());
    Require(fs::create_directory(directory), "Cannot create test directory");
    const auto path = directory / "pipeline.cert";
    const auto blocked = directory / "blocked";
    SCOPE_EXIT {
        std::error_code ignored;
        fs::remove(blocked / "keep", ignored);
        fs::remove(blocked, ignored);
        for (const auto& entry : fs::directory_iterator(directory)) {
            fs::remove(entry.path(), ignored);
        }
        fs::remove(directory, ignored);
    };

    Vulkan::Digest profile{}, snapshot{};
    for (size_t i = 0; i < profile.size(); ++i) {
        profile[i] = static_cast<u8>(i);
        snapshot[i] = static_cast<u8>(255 - i);
    }
    Require(!Vulkan::ReadPipelineCacheCertificate(path, profile, snapshot), "Missing file accepted");
    Require(Vulkan::WritePipelineCacheCertificate(path, profile, snapshot), "Initial write failed");
    Require(Vulkan::ReadPipelineCacheCertificate(path, profile, snapshot), "Roundtrip failed");
    auto other_profile = profile;
    auto other_snapshot = snapshot;
    other_profile[0] ^= 1;
    other_snapshot[31] ^= 1;
    Require(!Vulkan::ReadPipelineCacheCertificate(path, other_profile, snapshot), "Wrong profile accepted");
    Require(!Vulkan::ReadPipelineCacheCertificate(path, profile, other_snapshot), "Wrong snapshot accepted");

    std::ifstream file(path, std::ios::binary);
    const std::vector<char> original{std::istreambuf_iterator<char>{file}, {}};
    file.close();
    Require(original.size() == 76, "Unexpected certificate size");
    for (size_t size = 0; size < original.size(); ++size) {
        WriteBytes(path, {original.begin(), original.begin() + size});
        Require(!Vulkan::ReadPipelineCacheCertificate(path, profile, snapshot), "Truncated file accepted");
    }
    auto corrupt = original;
    corrupt.push_back(0);
    WriteBytes(path, corrupt);
    Require(!Vulkan::ReadPipelineCacheCertificate(path, profile, snapshot), "Trailing bytes accepted");
    for (size_t index = 0; index < original.size(); ++index) {
        corrupt = original;
        corrupt[index] ^= 1;
        WriteBytes(path, corrupt);
        Require(!Vulkan::ReadPipelineCacheCertificate(path, profile, snapshot), "Corrupt file accepted");
    }

    WriteBytes(path, original);
    Require(Vulkan::WritePipelineCacheCertificate(path, other_profile, other_snapshot), "Replacement failed");
    Require(Vulkan::ReadPipelineCacheCertificate(path, other_profile, other_snapshot), "Replacement unreadable");
    Require(!Vulkan::ReadPipelineCacheCertificate(path, profile, snapshot), "Replacement kept stale certificate");
    Require(!Vulkan::WritePipelineCacheCertificate(directory / "missing" / "file", profile, snapshot),
            "Write into missing parent succeeded");
    fs::create_directory(blocked);
    WriteBytes(blocked / "keep", {'o', 'k'});
    Require(!Vulkan::WritePipelineCacheCertificate(blocked, profile, snapshot), "Directory replaced");
    Require(fs::file_size(blocked / "keep") == 2, "Failed replacement damaged existing destination");
    Require(std::distance(fs::directory_iterator(directory), fs::directory_iterator{}) == 2,
            "Temporary file left behind");
    std::cout << "Pipeline cache certificate tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
