// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/memory_detect.h"

namespace Vulkan {

/// Slots for driver builds of native task/mesh graphics pipelines, which can each take minutes and
/// gigabytes. Measured on Mesa's Intel driver (ANV 26.2, Arc B580), Ocarina of Time: about 40 of
/// its cached pipelines are task/mesh pipelines, each 150-180 s and several GB in the compiler; the
/// boot's pipeline workers built them all at once after a SPIR-V change and the process passed
/// 73 GB. Other builds don't wait. Default: one slot per 8 GiB of a quarter of the host's RAM (1 to
/// 8); EDEN_NX2_HEAVY_BUILD_SLOTS=N sets them (0: no limit).
class HeavyBuildSlots {
public:
    /// Holds a slot while alive (none when the limit is off).
    class Guard {
    public:
        explicit Guard(HeavyBuildSlots& slots_) : slots{slots_.limit != 0 ? &slots_ : nullptr} {
            if (slots) {
                slots->Acquire();
            }
        }
        ~Guard() {
            if (slots) {
                slots->Release();
            }
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        HeavyBuildSlots* slots;
    };

    /// `limit_` slots, 0 for no limit (tests; the emulator uses Instance()).
    explicit HeavyBuildSlots(size_t limit_) : limit{limit_} {}

    static HeavyBuildSlots& Instance() {
        static HeavyBuildSlots instance{ChooseLimit()};
        return instance;
    }

    [[nodiscard]] size_t Limit() const noexcept {
        return limit;
    }

    /// The default number of slots for a host with `total_ram` bytes.
    [[nodiscard]] static constexpr size_t DefaultLimit(u64 total_ram) noexcept {
        constexpr u64 PerBuild = u64{8} << 30;
        return static_cast<size_t>(std::clamp<u64>(total_ram / 4 / PerBuild, 1, 8));
    }

private:
    static size_t ChooseLimit() {
        const size_t chosen = DefaultLimit(Common::GetMemInfo().TotalPhysicalMemory);
        const char* const value = std::getenv("EDEN_NX2_HEAVY_BUILD_SLOTS");
        if (value == nullptr || *value == '\0') {
            LOG_INFO(Render_Vulkan, "Task/mesh pipeline builds: at most {} at once", chosen);
            return chosen;
        }
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 0 || parsed > 64) {
            LOG_WARNING(Render_Vulkan,
                        "EDEN_NX2_HEAVY_BUILD_SLOTS={} is not a number from 0 to 64, using {}",
                        value, chosen);
            return chosen;
        }
        if (parsed == 0) {
            LOG_INFO(Render_Vulkan, "Task/mesh pipeline builds: no limit");
        } else {
            LOG_INFO(Render_Vulkan, "Task/mesh pipeline builds: at most {} at once", parsed);
        }
        return static_cast<size_t>(parsed);
    }

    void Acquire() {
        std::unique_lock lock{mutex};
        freed.wait(lock, [this] { return in_use < limit; });
        ++in_use;
    }

    void Release() {
        {
            std::scoped_lock lock{mutex};
            --in_use;
        }
        freed.notify_one();
    }

    const size_t limit;
    std::mutex mutex;
    std::condition_variable freed;
    size_t in_use{};
};

} // namespace Vulkan
