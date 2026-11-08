// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace Vulkan {

/// Values built once per key by whichever thread asks first; later askers share it, waiting while
/// it is built. Nothing is ever removed, so a key asked for again finds the value (or the failed
/// build's null) without building it twice. Holds the graphics pipeline libraries
/// (GraphicsPipelineLibraryCache), whose two parts of a new pipeline are built at once: one by a
/// prefetch on another thread (BuildIfAbsent), the other by the pipeline's own build (Get), which
/// takes the prefetched one over when the prefetch thread hasn't started it yet.
template <typename Value>
class BuildOnceMap {
public:
    enum class Outcome {
        /// This call built the value.
        Built,
        /// It was built before.
        Hit,
        /// Another thread was building it; this call waited.
        Waited,
        /// Another thread is building it; BuildIfAbsent left it to that thread.
        Busy,
    };

    /// The value for `key`, from `build()` on this thread when nobody has started it. A build
    /// that throws leaves null for the key (waiters wake up) and the exception goes on.
    template <typename Build>
    std::shared_ptr<const Value> Get(const std::string& key, Build&& build,
                                     Outcome* outcome = nullptr) {
        auto [entry, owner] = Claim(key);
        if (owner) {
            Run(*entry, std::forward<Build>(build));
            Report(outcome, Outcome::Built);
            return entry->value;
        }
        std::unique_lock lock{entry->mutex};
        Report(outcome, entry->done ? Outcome::Hit : Outcome::Waited);
        entry->condition.wait(lock, [&] { return entry->done; });
        return entry->value;
    }

    /// Builds the value for `key` on this thread when nobody has started it; returns at once when
    /// it is built or being built.
    template <typename Build>
    Outcome BuildIfAbsent(const std::string& key, Build&& build) {
        auto [entry, owner] = Claim(key);
        if (!owner) {
            std::scoped_lock lock{entry->mutex};
            return entry->done ? Outcome::Hit : Outcome::Busy;
        }
        Run(*entry, std::forward<Build>(build));
        return Outcome::Built;
    }

    /// Whether `key` was built (or failed) already.
    [[nodiscard]] bool Ready(const std::string& key) const {
        std::shared_ptr<Entry> entry;
        {
            std::scoped_lock lock{mutex};
            const auto it = entries.find(key);
            if (it == entries.end()) {
                return false;
            }
            entry = it->second;
        }
        std::scoped_lock lock{entry->mutex};
        return entry->done;
    }

    [[nodiscard]] size_t Size() const {
        std::scoped_lock lock{mutex};
        return entries.size();
    }

private:
    struct Entry {
        std::mutex mutex;
        std::condition_variable condition;
        bool done{};
        std::shared_ptr<const Value> value;
    };

    static void Report(Outcome* outcome, Outcome value) {
        if (outcome) {
            *outcome = value;
        }
    }

    std::pair<std::shared_ptr<Entry>, bool> Claim(const std::string& key) {
        std::scoped_lock lock{mutex};
        auto [it, inserted] = entries.try_emplace(key);
        if (inserted) {
            it->second = std::make_shared<Entry>();
        }
        return {it->second, inserted};
    }

    template <typename Build>
    static void Run(Entry& entry, Build&& build) {
        // Built outside the map lock: other keys are built at the same time on other threads.
        std::shared_ptr<const Value> value;
        try {
            value = build();
        } catch (...) {
            Finish(entry, nullptr);
            throw;
        }
        Finish(entry, std::move(value));
    }

    static void Finish(Entry& entry, std::shared_ptr<const Value> value) {
        {
            std::scoped_lock lock{entry.mutex};
            entry.value = std::move(value);
            entry.done = true;
        }
        entry.condition.notify_all();
    }

    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
};

} // namespace Vulkan
