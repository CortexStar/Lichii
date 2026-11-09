// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

namespace Vulkan {

/// The shader precache builds programs the game hasn't drawn yet, on every CPU thread, and a
/// driver compiler crash on one of them ends the whole process; the next start reaches the same
/// program again and crashes again (Mesa ANV 26.2 crashes in brw_instruction_scheduler compiling 3
/// of Ocarina of Time's ~60,000 warm-up fragment shaders). Each precache thread writes the id of
/// the build it is in to its slot of "<base>.inflight", flushed at once. At the next start the ids
/// left there join "<base>.poison" and are skipped from then on (a kill during a build leaves its
/// id there too: one program skipped). The poison list holds while `stamp` (the driver, device,
/// cache version and anything else the ids depend on) is unchanged: a new driver tries them again.
class PrecacheJournal {
public:
    PrecacheJournal(const std::filesystem::path& base, std::string stamp_, size_t slots,
                    const char* what)
        : inflight_path{WithSuffix(base, ".inflight")}, poison_path{WithSuffix(base, ".poison")},
          stamp{std::move(stamp_)} {
        {
            std::ifstream in(poison_path);
            std::string line;
            if (in && std::getline(in, line) && line == stamp) {
                u64 id{};
                while (in >> id) {
                    poisoned.insert(id);
                }
            }
        }
        size_t fresh = 0;
        {
            std::ifstream in(inflight_path, std::ios::binary);
            u64 id{};
            while (in.read(reinterpret_cast<char*>(&id), sizeof(id))) {
                if (id != Idle && poisoned.insert(id).second) {
                    ++fresh;
                }
            }
        }
        if (fresh != 0) {
            LOG_WARNING(Render_Vulkan,
                        "Shader precache: the last {} precache stopped during {} builds (a driver "
                        "crash?); they are skipped from now on with this driver",
                        what, fresh);
        }
        {
            std::ofstream out(poison_path, std::ios::trunc);
            out << stamp << '\n';
            for (const u64 id : poisoned) {
                out << id << '\n';
            }
        }
        if (!poisoned.empty()) {
            LOG_INFO(Render_Vulkan, "Shader precache: {} {} builds skipped (listed in {})",
                     poisoned.size(), what, Common::FS::PathToUTF8String(poison_path));
        }
        inflight.open(inflight_path, std::ios::binary | std::ios::out | std::ios::trunc);
        const std::vector<u64> idle(slots, Idle);
        inflight.write(reinterpret_cast<const char*>(idle.data()),
                       static_cast<std::streamsize>(idle.size() * sizeof(u64)));
        inflight.flush();
    }

    /// True for a build the driver crashed on (or may have) with this stamp.
    [[nodiscard]] bool Poisoned(u64 id) const {
        return poisoned.contains(id);
    }
    [[nodiscard]] size_t PoisonedCount() const {
        return poisoned.size();
    }
    void Begin(size_t slot, u64 id) {
        Write(slot, id);
    }
    void End(size_t slot) {
        Write(slot, Idle);
    }

    /// Ids never take this value.
    static constexpr u64 Idle = ~u64{0};

private:
    static std::filesystem::path WithSuffix(std::filesystem::path path, const char* suffix) {
        path += suffix;
        return path;
    }

    /// Flushed at once: the next start reads it after a crash of this process.
    void Write(size_t slot, u64 value) {
        std::scoped_lock lock{mutex};
        if (!inflight) {
            return;
        }
        inflight.seekp(static_cast<std::streamoff>(slot * sizeof(u64)));
        inflight.write(reinterpret_cast<const char*>(&value), sizeof(value));
        inflight.flush();
    }

    std::filesystem::path inflight_path;
    std::filesystem::path poison_path;
    std::string stamp;
    std::unordered_set<u64> poisoned; ///< Read-only once the threads start
    std::mutex mutex;
    std::ofstream inflight;
};

} // namespace Vulkan
