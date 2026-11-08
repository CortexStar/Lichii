// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "common/logging.h"
#include "common/thread_worker.h"
#include "video_core/renderer_vulkan/async_draw_policy.h"

namespace Vulkan {

class EmissionBudget {
public:
    class Reservation {
    public:
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        ~Reservation() {
            if (pending) {
                pending->fetch_sub(1, std::memory_order_relaxed);
            }
        }

    private:
        friend class EmissionBudget;
        Reservation() = default;
        std::shared_ptr<std::atomic<size_t>> pending;
    };

    explicit EmissionBudget(size_t capacity_) : capacity{capacity_} {}

    std::unique_ptr<Reservation> TryAcquire(bool native) {
        size_t count = pending->load(std::memory_order_relaxed);
        if (!DefersShaderEmission(native, count, capacity)) {
            return nullptr;
        }
        auto reservation = std::unique_ptr<Reservation>{new Reservation};
        while (DefersShaderEmission(native, count, capacity)) {
            if (pending->compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
                reservation->pending = pending;
                return reservation;
            }
        }
        return nullptr;
    }

    size_t Pending() const {
        return pending->load(std::memory_order_relaxed);
    }

private:
    const size_t capacity;
    std::shared_ptr<std::atomic<size_t>> pending = std::make_shared<std::atomic<size_t>>(0);
};

/// Builds a waiting thread ran itself and builders a waiter raised, for the runtime pipeline
/// report.
inline std::atomic<u32> builds_run_by_waiter{};
inline std::atomic<u32> builders_raised{};

/// Priority inheritance for a build on a ThreadPriority::Low builder thread: once a thread waits
/// for the build, the builder runs at RaisedPriority until the build is done, so threads of
/// normal priority (other programs) do not preempt the build the renderer is waiting on. Every
/// call is made under the build's mutex; the caller sets the priorities the calls ask for.
class BuilderPriority {
public:
    static constexpr Common::ThreadPriority RaisedPriority = Common::ThreadPriority::High;

    /// The thread that claimed the build, before it runs it.
    /// @return True when it has to raise itself: a waiter came first.
    [[nodiscard]] bool Start(bool low_priority_thread, Common::ThreadPriorityTarget thread) {
        started = true;
        low_priority = low_priority_thread;
        target = thread;
        return Raise();
    }

    /// A thread about to wait for the build.
    /// @return True when it has to raise Target(), the thread running the build.
    [[nodiscard]] bool Await() {
        awaited = true;
        return started && Raise();
    }

    /// The thread that ran the build, once it is done.
    /// @return True when it has to go back to ThreadPriority::Low.
    [[nodiscard]] bool Finish() {
        finished = true;
        return std::exchange(raised, false);
    }

    [[nodiscard]] Common::ThreadPriorityTarget Target() const {
        return target;
    }

private:
    bool Raise() {
        if (!low_priority || !awaited || raised || finished) {
            return false;
        }
        raised = true;
        return true;
    }

    Common::ThreadPriorityTarget target{};
    bool started{};
    bool low_priority{};
    bool awaited{};
    bool raised{};
    bool finished{};
};

class PipelineBuild {
    struct State {
        explicit State(std::function<void()> task_) : task{std::move(task_)} {}

        /// @return True when this call ran the build.
        bool Run(bool low_priority_thread = false) noexcept {
            if (claimed.exchange(true)) {
                return false;
            }
            {
                std::scoped_lock lock{mutex};
                if (priority.Start(low_priority_thread, Common::CurrentThreadPriorityTarget())) {
                    Common::SetCurrentThreadPriority(BuilderPriority::RaisedPriority);
                    builders_raised.fetch_add(1, std::memory_order_relaxed);
                }
            }
            try {
                task();
            } catch (const std::exception& exception) {
                failed = true;
                LOG_ERROR(Render_Vulkan, "Graphics pipeline worker failed: {}", exception.what());
            } catch (...) {
                failed = true;
                LOG_ERROR(Render_Vulkan,
                          "Graphics pipeline worker failed with an unknown exception");
            }
            task = {};
            bool lower;
            {
                std::scoped_lock lock{mutex};
                complete.store(true, std::memory_order_release);
                lower = priority.Finish();
                condition.notify_all();
            }
            if (lower) {
                Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            }
            return true;
        }

        /// Raises the builder for a waiter; called under the mutex.
        void Await() {
            if (!complete.load(std::memory_order_relaxed) && priority.Await()) {
                Common::SetOtherThreadPriority(priority.Target(), BuilderPriority::RaisedPriority);
                builders_raised.fetch_add(1, std::memory_order_relaxed);
            }
        }

        std::function<void()> task;
        bool failed{};
        std::atomic_bool claimed{};
        std::atomic_bool complete{};
        std::mutex mutex;
        std::condition_variable condition;
        BuilderPriority priority;
    };

public:
    PipelineBuild(Common::ThreadWorker* workers, std::function<void()> task)
        : state{std::make_shared<State>(std::move(task))} {
        if (workers) {
            workers->QueueWork(
                [state = state, low = workers->LowersPriority()] { state->Run(low); });
        } else {
            state->Run();
        }
    }

    ~PipelineBuild() {
        Wait(true);
    }

    bool IsBuilt() const {
        return state->complete.load(std::memory_order_acquire);
    }

    bool Failed() const {
        return IsBuilt() && state->failed;
    }

    void Wait(bool may_build = false) const {
        const auto shared = state;
        if (may_build && shared->Run()) {
            builds_run_by_waiter.fetch_add(1, std::memory_order_relaxed);
        }
        std::unique_lock lock{shared->mutex};
        shared->Await();
        shared->condition.wait(lock,
                               [&] { return shared->complete.load(std::memory_order_acquire); });
    }

    /// Waits up to `timeout` for the build, without running it on this thread. The worker sets
    /// `complete` under the mutex before notifying, so the wakeup cannot be lost.
    /// @return True when the pipeline is built.
    bool WaitFor(std::chrono::microseconds timeout) const {
        const auto shared = state;
        std::unique_lock lock{shared->mutex};
        shared->Await();
        return shared->condition.wait_for(lock, timeout, [&] {
            return shared->complete.load(std::memory_order_acquire);
        });
    }

    PipelineBuild(const PipelineBuild&) = delete;
    PipelineBuild& operator=(const PipelineBuild&) = delete;

private:
    std::shared_ptr<State> state;
};

} // namespace Vulkan
