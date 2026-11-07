// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>

#include "common/common_types.h"

namespace Vulkan {

using Digest = std::array<u8, 32>;

[[nodiscard]] bool ReadPipelineCacheCertificate(const std::filesystem::path& path,
                                               const Digest& expected_profile_digest,
                                               const Digest& expected_snapshot_digest);

[[nodiscard]] bool WritePipelineCacheCertificate(const std::filesystem::path& path,
                                                const Digest& profile_digest,
                                                const Digest& snapshot_digest);

} // namespace Vulkan
