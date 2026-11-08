// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

/// Called when a driver fails a pipeline build. For VK_ERROR_OUT_OF_HOST_MEMORY, logs once per
/// session what it means: on Windows the commit limit (RAM plus page file) is reached, which a
/// Switch 2 game's 14 GiB of committed guest memory makes likely on 16 GB hosts (#462).
void ReportPipelineBuildFailure(VkResult result);

} // namespace Vulkan
