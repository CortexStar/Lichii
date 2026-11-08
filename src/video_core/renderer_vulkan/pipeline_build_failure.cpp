// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>

#include "common/logging.h"
#include "video_core/renderer_vulkan/pipeline_build_failure.h"

namespace Vulkan {

void ReportPipelineBuildFailure(VkResult result) {
    static std::atomic_flag reported;
    if (result != VK_ERROR_OUT_OF_HOST_MEMORY || reported.test_and_set()) {
        return;
    }
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        // ullTotalPageFile and ullAvailPageFile are the system commit limit and what is left of it.
        constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
        LOG_CRITICAL(Render_Vulkan,
                     "The GPU driver ran out of host memory building a pipeline: Windows commit is "
                     "{:.1f} of {:.1f} GiB. The commit limit is RAM plus the page file, and a "
                     "Switch 2 game commits 14 GiB of guest memory when it starts. Enlarge the "
                     "page file; until then, pipelines that fail draw nothing.",
                     static_cast<double>(status.ullTotalPageFile - status.ullAvailPageFile) / GiB,
                     static_cast<double>(status.ullTotalPageFile) / GiB);
        return;
    }
#endif
    LOG_CRITICAL(Render_Vulkan,
                 "The GPU driver ran out of host memory building a pipeline. Close other programs "
                 "or add swap; until then, pipelines that fail draw nothing.");
}

} // namespace Vulkan
