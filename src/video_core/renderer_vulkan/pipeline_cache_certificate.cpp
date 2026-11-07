// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/pipeline_cache_certificate.h"

#include <algorithm>
#include <exception>
#include <fstream>

#include "common/scope_exit.h"
#include "common/uuid.h"

namespace Vulkan {
namespace {

constexpr std::array<u8, 12> Header{'L', 'C', 'H', 'P', 'C', 'E', 'R', 'T', 1, 0, 0, 0};
using Certificate = std::array<u8, Header.size() + 2 * Digest{}.size()>;

Certificate Encode(const Digest& profile, const Digest& snapshot) {
    Certificate result{};
    auto next = std::copy(Header.begin(), Header.end(), result.begin());
    next = std::copy(profile.begin(), profile.end(), next);
    std::copy(snapshot.begin(), snapshot.end(), next);
    return result;
}

} // namespace

bool ReadPipelineCacheCertificate(const std::filesystem::path& path,
                                  const Digest& expected_profile_digest,
                                  const Digest& expected_snapshot_digest) try {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Certificate actual{};
    if (!file || file.tellg() != static_cast<std::streamoff>(actual.size())) {
        return false;
    }
    file.seekg(0);
    file.read(reinterpret_cast<char*>(actual.data()), static_cast<std::streamsize>(actual.size()));
    return file && actual == Encode(expected_profile_digest, expected_snapshot_digest);
} catch (const std::exception&) {
    return false;
}

bool WritePipelineCacheCertificate(const std::filesystem::path& path,
                                   const Digest& profile_digest,
                                   const Digest& snapshot_digest) try {
    auto temporary = path;
    temporary += "." + Common::UUID::MakeRandom().RawString() + ".tmp";
    SCOPE_EXIT {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
    };
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    const auto certificate = Encode(profile_digest, snapshot_digest);
    file.write(reinterpret_cast<const char*>(certificate.data()),
               static_cast<std::streamsize>(certificate.size()));
    file.close();

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
} catch (const std::exception&) {
    return false;
}

} // namespace Vulkan
