// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/unique_function.h"
#include "video_core/renderer_vulkan/pipeline_build.h"

namespace Vulkan {

/// GPU-thread bookkeeping: an adopted cached build may fail after metadata was published.
/// Allow one live-environment retry without turning normal runtime failures into a retry loop.
template <typename Pipeline>
class CachedPipelineRetries {
public:
    void Mark(Pipeline* pipeline) {
        if (pipeline) {
            cached.insert(pipeline);
        }
    }
    bool NeedsRetry(Pipeline* pipeline) const {
        return pipeline && cached.contains(pipeline) && pipeline->Failed();
    }
    bool Consume(Pipeline* pipeline) {
        if (!NeedsRetry(pipeline)) {
            return false;
        }
        cached.erase(pipeline);
        return true;
    }
    void Forget(Pipeline* pipeline) {
        cached.erase(pipeline);
    }

private:
    std::unordered_set<Pipeline*> cached;
};

template <typename Key, typename Pipeline>
class PipelinePrebuild {
public:
    // Capture lightweight input references here; materialize shader environments inside the
    // callback. Return once pipeline metadata exists, without waiting for driver compilation.
    using Build = Common::UniqueFunction<std::unique_ptr<Pipeline>>;

private:
    struct Entry {
        Key key;
        size_t capture_bytes{};
        Build build;
        std::unique_ptr<Pipeline> pipeline;
        bool started{};
        bool complete{};
        bool demanded{};
        bool slot_reserved{};
        bool capture_reserved{};
    };

public:
    PipelinePrebuild(size_t capacity_, size_t capture_limit_, bool broken_parallel_compilation)
        : capacity{broken_parallel_compilation ? 0 : capacity_}, capture_limit{capture_limit_} {}

    ~PipelinePrebuild() {
        Stop();
    }

    bool Add(const Key& key, size_t capture_bytes, Build build) {
        std::scoped_lock lock{mutex};
        if (stopped || capacity == 0 || entries.contains(key)) {
            return false;
        }
        auto entry = std::make_shared<Entry>();
        entry->key = key;
        entry->capture_bytes = capture_bytes;
        entry->build = std::move(build);
        entries.emplace(key, entry);
        pending.push_back(std::move(entry));
        ScheduleLocked();
        return true;
    }

    void Start(size_t threads) {
        std::scoped_lock lock{mutex};
        if (stopped || workers || threads == 0 || capacity == 0) {
            return;
        }
        workers.reset(new Common::ThreadWorker(threads, "VkPipelinePrebuild", {},
                                               Common::ThreadPlacement::Background));
        ScheduleLocked();
    }

    std::unique_ptr<Pipeline> Take(const Key& key) {
        std::unique_lock lock{mutex};
        const auto it = entries.find(key);
        if (stopped || it == entries.end()) {
            return nullptr;
        }
        const auto entry = it->second;
        ++taking;
        entries.erase(it);
        entry->demanded = true;
        if (!entry->started) {
            // A demanded job can claim a queued callback. Its later worker callback is a no-op.
            lock.unlock();
            Run(entry);
            lock.lock();
        }
        condition.wait(lock, [&] { return entry->complete || stopped; });
        std::unique_ptr<Pipeline> pipeline;
        if (entry->complete) {
            ReleaseSlotLocked(*entry);
            ScheduleLocked();
            pipeline = std::move(entry->pipeline);
        }
        --taking;
        condition.notify_all();
        return pipeline;
    }

    void Stop() {
        std::unique_ptr<Common::ThreadWorker> stopping_workers;
        {
            std::scoped_lock lock{mutex};
            stopped = true;
            stopping_workers = std::move(workers);
            condition.notify_all();
        }
        stopping_workers.reset();
        std::unique_lock lock{mutex};
        condition.wait(lock, [&] { return running == 0 && taking == 0; });
        auto abandoned = std::move(pending);
        // Pipeline destruction may wait for a driver build; never hold the cache mutex there.
        auto remaining = std::move(entries);
        lock.unlock();
    }

    /// Drain the previous load before reopening admission with fresh budgets. The owner must
    /// serialize Start(), Stop() and ResetForLoad(); demand may still be finishing during Stop().
    void ResetForLoad() {
        Stop();
        std::scoped_lock lock{mutex};
        resident = 0;
        captured_bytes = 0;
        stopped = false;
    }

private:
    void ReleaseSlotLocked(Entry& entry) {
        if (std::exchange(entry.slot_reserved, false)) {
            --resident;
        }
    }

    void ScheduleLocked() {
        if (stopped || !workers) {
            return;
        }
        // A finished pipeline retains its resident slot until adopted. Only the small disk
        // references for overflow wait here; captured environments are bounded independently.
        size_t remaining = pending.size();
        while (remaining-- != 0 && resident < capacity) {
            auto entry = std::move(pending.front());
            pending.pop_front();
            if (entry->started || entry->demanded) {
                continue;
            }
            if (entry->capture_bytes > capture_limit - captured_bytes) {
                pending.push_back(std::move(entry));
                continue;
            }
            ++resident;
            captured_bytes += entry->capture_bytes;
            entry->slot_reserved = true;
            entry->capture_reserved = true;
            workers->QueueWork([this, entry = std::move(entry)] { Run(entry); });
        }
    }

    void Run(const std::shared_ptr<Entry>& entry) {
        Build build;
        {
            std::scoped_lock lock{mutex};
            if (entry->started || stopped) {
                return;
            }
            entry->started = true;
            ++running;
            build = std::move(entry->build);
        }
        std::unique_ptr<Pipeline> pipeline;
        try {
            pipeline = build();
        } catch (const std::exception& exception) {
            LOG_ERROR(Render_Vulkan, "Cached pipeline prebuild failed: {}", exception.what());
        } catch (...) {
            LOG_ERROR(Render_Vulkan, "Cached pipeline prebuild failed");
        }
        build = {};
        {
            std::scoped_lock lock{mutex};
            entry->pipeline = std::move(pipeline);
            entry->complete = true;
            --running;
            if (std::exchange(entry->capture_reserved, false)) {
                captured_bytes -= entry->capture_bytes;
            }
            if (entry->demanded || !entry->pipeline) {
                ReleaseSlotLocked(*entry);
            }
            if (!entry->demanded && !entry->pipeline) {
                entries.erase(entry->key);
            }
            ScheduleLocked();
            condition.notify_all();
        }
    }

    const size_t capacity;
    const size_t capture_limit;
    size_t captured_bytes{};
    size_t resident{};
    size_t running{};
    size_t taking{};
    bool stopped{};
    std::mutex mutex;
    std::condition_variable condition;
    std::unordered_map<Key, std::shared_ptr<Entry>> entries;
    std::deque<std::shared_ptr<Entry>> pending;
    std::unique_ptr<Common::ThreadWorker> workers;
};

} // namespace Vulkan
