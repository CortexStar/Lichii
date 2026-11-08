// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <boost/heap/fibonacci_heap.hpp>

#include "common/common_types.h"
#include "common/thread.h"
#include "common/cpu_features.h"

namespace Core::Timing {

/// A callback that may be scheduled for a particular core timing event.
using TimedCallback = std::function<std::optional<std::chrono::nanoseconds>(
    s64 time, std::chrono::nanoseconds ns_late)>;

/// Contains the characteristics of a particular event.
struct EventType {
    explicit EventType(TimedCallback&& callback_, std::string&& name_, bool relaxed_ = false)
        : callback{std::move(callback_)}, name{std::move(name_)}, sequence_number{0},
          relaxed{relaxed_} {}

    /// The event's callback function.
    TimedCallback callback;
    /// A pointer to the name of the event.
    const std::string name;
    /// A monotonic sequence number, incremented when this event is
    /// changed externally.
    size_t sequence_number;
    /// The event may run up to an OS timer tick late (a poll, not a deadline): the timer thread
    /// sleeps in the OS until it instead of waiting for it in a monitor wait. Its owner may change
    /// it (SetRelaxed), for example from the callback: it applies from the next time the event
    /// is scheduled or rescheduled, and the time already queued keeps the old choice.
    std::atomic<bool> relaxed;

    /// Changes `relaxed` for the event's next schedule (a looping event: its next run).
    void SetRelaxed(bool value) {
        relaxed.store(value, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsRelaxed() const {
        return relaxed.load(std::memory_order_relaxed);
    }
};

enum class UnscheduleEventType {
    Wait,
    NoWait,
};

/**
 * This is a system to schedule events into the emulated machine's future. Time is measured
 * in main CPU clock cycles.
 *
 * To schedule an event, you first have to register its type. This is where you pass in the
 * callback. You then schedule events using the type ID you get back.
 *
 * The s64 ns_late that the callbacks get is how many ns late it was.
 * So to schedule a new event on a regular basis:
 * inside callback:
 *   ScheduleEvent(period_in_ns - ns_late, callback, "whatever")
 */
class CoreTiming {
public:
    CoreTiming();
    ~CoreTiming();

    CoreTiming(const CoreTiming&) = delete;
    CoreTiming(CoreTiming&&) = delete;

    CoreTiming& operator=(const CoreTiming&) = delete;
    CoreTiming& operator=(CoreTiming&&) = delete;

    /// CoreTiming begins at the boundary of timing slice -1. An initial call to Advance() is
    /// required to end slice - 1 and start slice 0 before the first cycle of code is executed.
    void Initialize(std::function<void()>&& on_thread_init_);

    /// Clear all pending events. This should ONLY be done on exit.
    void ClearPendingEvents();

    /// Sets if emulation is multicore or single core, must be set before Initialize
    void SetMulticore(bool is_multicore_) {
        is_multicore = is_multicore_;
    }

    /// Pauses/Unpauses the execution of the timer thread.
    void Pause(bool is_paused);

    /// Pauses/Unpauses the execution of the timer thread and waits until paused.
    void SyncPause(bool is_paused);

    /// Checks if core timing is running.
    bool IsRunning() const;

    /// Checks if the timer thread has started.
    bool HasStarted() const {
        return has_started;
    }

    /// Checks if there are any pending time events.
    bool HasPendingEvents() const;

    /// Schedules an event in core timing
    void ScheduleEvent(std::chrono::nanoseconds ns_into_future,
                       const std::shared_ptr<EventType>& event_type, bool absolute_time = false);

    /// Schedules an event which will automatically re-schedule itself with the given time, until
    /// unscheduled
    void ScheduleLoopingEvent(std::chrono::nanoseconds start_time,
                              std::chrono::nanoseconds resched_time,
                              const std::shared_ptr<EventType>& event_type,
                              bool absolute_time = false);

    void UnscheduleEvent(const std::shared_ptr<EventType>& event_type,
                         UnscheduleEventType type = UnscheduleEventType::Wait);

    void AddTicks(u64 ticks_to_add);

    void ResetTicks();

    void Idle();

    s64 GetDowncount() const noexcept {
        return downcount;
    }

    /// Returns the current CNTPCT tick value.
    u64 GetClockTicks() const;

    /// Whether GetClockTicks is the host clock's guest counter (WallClock::GetCNTPCT) unchanged:
    /// multicore on the native (invariant TSC) clock, no synchronized core speed, and no CPU clock
    /// scaling (Switch 2 titles ignore it). The JIT then reads the counter inline.
    static constexpr bool GuestCounterIsHostClock(bool multicore, bool native_clock,
                                                  bool sync_core_speed, bool switch2) {
        return multicore && native_clock && !sync_core_speed && switch2;
    }
    bool GuestCounterIsHostClock() const;

    /// Returns the current GPU tick value.
    u64 GetGPUTicks() const;

    [[nodiscard]] std::chrono::microseconds GetGlobalTimeUs() const noexcept;
    [[nodiscard]] std::chrono::nanoseconds GetGlobalTimeNs() const noexcept;

    /// Global time (GetGlobalTimeNs) at which emulation last resumed from a pause.
    [[nodiscard]] std::chrono::nanoseconds GetPauseEndTime() const noexcept {
        return std::chrono::nanoseconds{pause_end_time.load(std::memory_order_relaxed)};
    }

    /// Checks for events manually and returns time in nanoseconds for next event, threadsafe.
    std::optional<s64> Advance();

#ifdef _WIN32
    void SetTimerResolutionNs(std::chrono::nanoseconds ns);
#endif

    struct Event;
    struct TimingTrace;

    void Reset();

    /// Whether the timer thread must be woken for an event at `time` (basic_lock held).
    [[nodiscard]] bool NeedsWake(s64 time, bool relaxed) const;

    using heap_t = boost::heap::fibonacci_heap<CoreTiming::Event, boost::heap::compare<std::greater<>>>;
    heap_t event_queue;
    s64 global_timer = 0;
#ifdef _WIN32
    /// The OS timer tick the frontend set (Advance's OS wake guard), 0 until it sets one.
    std::atomic<s64> timer_resolution_ns{};
#endif
    /// Set by Advance for the timer thread: wait until `until`, in the OS (blocks) or precisely.
    struct NextWait {
        s64 until{};
        bool blocks{};
        std::string name; ///< The event it waits for (EDEN_NX2_TIMING_TRACE only)
    } next_wait;
    /// When the timer thread looks at the queue next: the minimum while it is in Advance, the
    /// maximum while it waits on an empty queue (basic_lock).
    s64 planned_wake{(std::numeric_limits<s64>::min)()};
    /// How late an OS wait of the timer thread can end (Advance; see Common::OsWakeTime).
    s64 os_wake_guard{};
    /// Tests: the guard Advance uses instead of the OS's timer tick (negative: the OS's).
    s64 os_wake_guard_override{-1};
    /// EDEN_NX2_TIMING_TRACE=1: what the timer thread waits for and runs, logged every 2 s.
    std::unique_ptr<TimingTrace> trace;
    u64 event_fifo_id = 0;
    std::atomic<s64> pause_end_time{};
    /// Cycle timing
    u64 cpu_ticks{};
    s64 downcount{};
    Common::Event event{};
    Common::Event pause_event{};
    std::function<void()> on_thread_init{};
    std::jthread timer_thread;
    mutable std::mutex basic_lock;
    std::mutex advance_lock;
    std::atomic<bool> paused{};
    std::atomic<bool> paused_set{};
    std::atomic<bool> wait_set{};
    std::atomic<bool> has_started{};
    bool is_multicore{};
};

/// Creates a core timing event with the given name and callback.
///
/// @param name     The name of the core timing event to create.
/// @param callback The callback to execute for the event.
///
/// @returns An EventType instance representing the created event.
///
std::shared_ptr<EventType> CreateEvent(std::string name, TimedCallback&& callback);

/// Creates a relaxed event (EventType::relaxed): one that may run up to an OS timer tick late.
std::shared_ptr<EventType> CreateRelaxedEvent(std::string name, TimedCallback&& callback);

} // namespace Core::Timing
