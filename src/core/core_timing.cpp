// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include "common/cpu_features.h"
#include "common/event_wait.h"
#include "common/logging.h"

#ifdef _WIN32
#include "common/windows/timer_resolution.h"
#endif

#include "common/settings.h"
#include "core/core_timing.h"
#include "core/hardware_properties.h"

namespace Core::Timing {

constexpr s64 MAX_SLICE_LENGTH = 10000;

constexpr u32 CPU_CLOCK_BASE_MHZ = 1020;
constexpr u32 CPU_CLOCK_BOOST_MHZ = 1734;
constexpr u32 CPU_CLOCK_OVERCLOCK_MHZ = 2040;

constexpr u32 CpuClockTargetMhz(Settings::CpuClock clock) {
    switch (clock) {
    case Settings::CpuClock::Boost:
        return CPU_CLOCK_BOOST_MHZ;
    case Settings::CpuClock::Overclock:
        return CPU_CLOCK_OVERCLOCK_MHZ;
    default:
        return CPU_CLOCK_BASE_MHZ;
    }
}

std::shared_ptr<EventType> CreateEvent(std::string name, TimedCallback&& callback) {
    return std::make_shared<EventType>(std::move(callback), std::move(name));
}

std::shared_ptr<EventType> CreateRelaxedEvent(std::string name, TimedCallback&& callback) {
    return std::make_shared<EventType>(std::move(callback), std::move(name), true);
}

namespace {
// How late an OS wait of the timer thread can end: one timer tick (0.5 ms at the maximum
// resolution) plus the time to dispatch the woken thread. Measured on Windows 11 with a 0.5 ms
// tick: a high-resolution waitable timer fires on the next tick, 13-50 us after it.
s64 OsWakeGuardNs() {
#ifdef _WIN32
    return Common::Windows::GetCurrentTimerResolution().count() + 100'000;
#else
    // Elsewhere the timed wait is a condition variable wait either way.
    return 0;
#endif
}
} // Anonymous namespace

struct CoreTiming::Event {
    s64 time;
    u64 fifo_order;
    std::weak_ptr<EventType> type;
    s64 reschedule_time;
    heap_t::handle_type handle{};
    bool relaxed{};

    // Sort by time, unless the times are the same, in which case sort by
    // the order added to the queue
    friend bool operator>(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) > std::tie(right.time, right.fifo_order);
    }

    friend bool operator<(const Event& left, const Event& right) {
        return std::tie(left.time, left.fifo_order) < std::tie(right.time, right.fifo_order);
    }
};

struct CoreTiming::TimingTrace {
    struct EventCounters {
        u64 callbacks{};
        s64 callback_ns{};
        s64 late_ns{};
        u64 schedules{};
        u64 wakes{};
    };
    struct WaitCounters {
        u64 count{};
        s64 ns{};
        u64 woken{}; ///< Ended by Set, not by the timeout
    };

    void NoteCallback(std::string_view name, s64 late, s64 ns) {
        std::scoped_lock lk{mutex};
        auto& counters = Find(events, name);
        ++counters.callbacks;
        counters.callback_ns += ns;
        counters.late_ns += late;
    }

    void NoteSchedule(std::string_view name, bool wake) {
        std::scoped_lock lk{mutex};
        auto& counters = Find(events, name);
        ++counters.schedules;
        counters.wakes += wake ? 1 : 0;
    }

    void NoteWait(bool blocks, bool woken, std::string_view name, s64 ns) {
        std::scoped_lock lk{mutex};
        auto& total = blocks ? blocked : precise;
        ++total.count;
        total.ns += ns;
        total.woken += woken ? 1 : 0;
        if (!blocks) {
            auto& by_event = Find(precise_by_event, name);
            ++by_event.count;
            by_event.ns += ns;
            by_event.woken += woken ? 1 : 0;
        }
    }

    /// From the timer thread: logs and clears the counters every 2 s.
    void MaybeReport(s64 now) {
        constexpr s64 Window = 2'000'000'000;
        if (window_start == 0) {
            window_start = now;
        }
        if (now - window_start < Window) {
            return;
        }
        std::scoped_lock lk{mutex};
        const double seconds = static_cast<double>(now - window_start) / 1e9;
        const auto ms = [](s64 ns) { return static_cast<double>(ns) / 1e6; };
        LOG_INFO(Core_Timing,
                 "Timing trace {:.2f} s: OS waits {} ({:.1f} ms, {} ended by Set), precise waits "
                 "{} ({:.1f} ms, {} ended by Set)",
                 seconds, blocked.count, ms(blocked.ns), blocked.woken, precise.count,
                 ms(precise.ns), precise.woken);
        for (const auto& [name, counters] : precise_by_event) {
            LOG_INFO(Core_Timing, "  precise wait for {}: {} ({:.1f} ms, {} ended by Set)", name,
                     counters.count, ms(counters.ns), counters.woken);
        }
        for (const auto& [name, counters] : events) {
            LOG_INFO(Core_Timing,
                     "  event {}: {} callbacks ({:.2f} ms, {:.1f} us late on average), {} "
                     "scheduled, {} woke the timer thread",
                     name, counters.callbacks, ms(counters.callback_ns),
                     counters.callbacks != 0 ? static_cast<double>(counters.late_ns) / 1e3 /
                                                   static_cast<double>(counters.callbacks)
                                             : 0.0,
                     counters.schedules, counters.wakes);
        }
        events.clear();
        precise_by_event.clear();
        blocked = {};
        precise = {};
        window_start = now;
    }

private:
    template <typename Counters>
    static Counters& Find(std::map<std::string, Counters, std::less<>>& map,
                          std::string_view name) {
        if (const auto it = map.find(name); it != map.end()) {
            return it->second;
        }
        return map.emplace(std::string{name}, Counters{}).first->second;
    }

    std::mutex mutex;
    std::map<std::string, EventCounters, std::less<>> events;
    std::map<std::string, WaitCounters, std::less<>> precise_by_event;
    WaitCounters blocked;
    WaitCounters precise;
    s64 window_start{};
};

CoreTiming::CoreTiming() = default;
CoreTiming::~CoreTiming() {
    Reset();
}

void CoreTiming::Initialize(std::function<void()>&& on_thread_init_) {
    Reset();
    on_thread_init = std::move(on_thread_init_);
    event_fifo_id = 0;
    cpu_ticks = 0;
    if (std::getenv("EDEN_NX2_TIMING_TRACE") != nullptr && !trace) {
        trace = std::make_unique<TimingTrace>();
    }
    if (is_multicore) {
        timer_thread = std::jthread([this](std::stop_token stop_token) {
            Common::SetCurrentThreadName("HostTiming");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
            Common::SetCurrentThreadToPerformanceCores();
            on_thread_init();
            has_started = true;

            // base frequency in MHz: 1ns (10^-9) = 1GHz (10^9)
            while (!stop_token.stop_requested()) {
                while (!paused && !stop_token.stop_requested()) {
                    paused_set = false;
                    if (auto const next_time = Advance(); next_time) {
                        // There are more events left in the queue. Sleep in the OS while no
                        // event needs a precise wake-up, then wait precisely for the next one:
                        // a monitor wait counts as CPU time for as long as it lasts. Advance
                        // chose which (next_wait) and told ScheduleEvent when to wake us.
                        auto const remaining = next_wait.until - GetGlobalTimeNs().count();
                        if (remaining > 0) {
                            const auto begin = trace ? GetGlobalTimeNs().count() : 0;
                            const bool woken =
                                next_wait.blocks
                                    ? event.BlockFor(std::chrono::nanoseconds(remaining))
                                    : event.WaitFor(std::chrono::nanoseconds(remaining));
                            if (trace) {
                                const s64 end = GetGlobalTimeNs().count();
                                trace->NoteWait(next_wait.blocks, woken, next_wait.name,
                                                end - begin);
                                trace->MaybeReport(end);
                            }
                        }
                    } else {
                        // Queue is empty, wait until another event is scheduled and signals us to
                        // continue.
                        wait_set = true;
                        event.Wait();
                    }
                    wait_set = false;
                }
                paused_set = true;
                pause_event.Wait();
            }
        });
    }
}

void CoreTiming::ClearPendingEvents() {
    std::scoped_lock lock{advance_lock, basic_lock};
    event_queue.clear();
    event.Set();
}

void CoreTiming::Pause(bool is_paused) {
    paused = is_paused;
    pause_event.Set();

    if (!is_paused) {
        pause_end_time = GetGlobalTimeNs().count();
    }
}

void CoreTiming::SyncPause(bool is_paused) {
    if (is_paused == paused && paused_set == paused) {
        return;
    }

    Pause(is_paused);
    if (timer_thread.joinable()) {
        if (!is_paused) {
            pause_event.Set();
        }
        event.Set();
        while (paused_set != is_paused)
            ;
    }

    if (!is_paused) {
        pause_end_time = GetGlobalTimeNs().count();
    }
}

bool CoreTiming::IsRunning() const {
    return !paused_set;
}

bool CoreTiming::HasPendingEvents() const {
    std::scoped_lock lock{basic_lock};
    return !(wait_set && event_queue.empty());
}

void CoreTiming::ScheduleEvent(std::chrono::nanoseconds ns_into_future,
                               const std::shared_ptr<EventType>& event_type, bool absolute_time) {
    bool wake;
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? ns_into_future : GetGlobalTimeNs() + ns_into_future};
        const bool relaxed = event_type->IsRelaxed();

        auto h{event_queue.emplace(Event{next_time.count(), event_fifo_id++, event_type, 0,
                                         heap_t::handle_type{}, relaxed})};
        (*h).handle = h;
        wake = NeedsWake(next_time.count(), relaxed);
    }
    if (trace) {
        trace->NoteSchedule(event_type->name, wake);
    }
    if (wake) {
        event.Set();
    }
}

void CoreTiming::ScheduleLoopingEvent(std::chrono::nanoseconds start_time,
                                      std::chrono::nanoseconds resched_time,
                                      const std::shared_ptr<EventType>& event_type,
                                      bool absolute_time) {
    bool wake;
    {
        std::scoped_lock scope{basic_lock};
        const auto next_time{absolute_time ? start_time : GetGlobalTimeNs() + start_time};
        const bool relaxed = event_type->IsRelaxed();

        auto h{event_queue.emplace(Event{next_time.count(), event_fifo_id++, event_type,
                                         resched_time.count(), heap_t::handle_type{}, relaxed})};
        (*h).handle = h;
        wake = NeedsWake(next_time.count(), relaxed);
    }
    if (trace) {
        trace->NoteSchedule(event_type->name, wake);
    }
    if (wake) {
        event.Set();
    }
}

bool CoreTiming::NeedsWake(s64 time, bool relaxed) const {
    // The timer thread looks at the queue again by planned_wake (Advance), or right away while it
    // is in Advance (planned_wake is then the minimum). The new event only needs it sooner if the
    // thread has to be up before then to wait for it.
    return Common::OsWakeTime(time, relaxed, os_wake_guard) < planned_wake;
}

void CoreTiming::UnscheduleEvent(const std::shared_ptr<EventType>& event_type,
                                 UnscheduleEventType type) {
    {
        std::scoped_lock lk{basic_lock};

        std::vector<heap_t::handle_type> to_remove;
        for (auto itr = event_queue.begin(); itr != event_queue.end(); itr++) {
            const Event& e = *itr;
            if (e.type.lock().get() == event_type.get()) {
                to_remove.push_back(itr->handle);
            }
        }

        for (auto& h : to_remove) {
            event_queue.erase(h);
        }

        event_type->sequence_number++;
    }

    // Force any in-progress events to finish
    if (type == UnscheduleEventType::Wait) {
        std::scoped_lock lk{advance_lock};
    }
}

static u64 GetNextTickCount(u64 next_ticks) {
    if (Settings::values.use_custom_cpu_ticks.GetValue()) {
        return Settings::values.cpu_ticks.GetValue();
    }
    return next_ticks;
}

void CoreTiming::AddTicks(u64 ticks_to_add) {
    const u64 ticks = GetNextTickCount(ticks_to_add);
    cpu_ticks += ticks;
    downcount -= static_cast<s64>(ticks);
}

void CoreTiming::Idle() {
    AddTicks(1000U);
}

void CoreTiming::ResetTicks() {
    downcount = MAX_SLICE_LENGTH;
}

bool CoreTiming::GuestCounterIsHostClock() const {
    return GuestCounterIsHostClock(is_multicore, Common::g_wall_clock.IsNative(),
                                   Settings::values.sync_core_speed.GetValue(),
                                   Settings::CurrentProgramIsSwitch2());
}

u64 CoreTiming::GetClockTicks() const {
    u64 fres = is_multicore ? Common::g_wall_clock.GetCNTPCT() : Common::g_wall_clock.CPUTickToGuestCNTPCT(cpu_ticks);
    // Switch 2 SDKs convert ticks with a fixed 31.25 MHz, so a scaled counter speeds up every
    // tick-based time (Ocarina with the Dynamic FPS add-on ran ~1.7x fast on Boost).
    if (const u32 target = CpuClockTargetMhz(Settings::values.cpu_clock.GetValue());
        target != CPU_CLOCK_BASE_MHZ && !Settings::CurrentProgramIsSwitch2()) {
        fres = fres * target / CPU_CLOCK_BASE_MHZ;
    }
    if (::Settings::values.sync_core_speed.GetValue()) {
        auto const ticks = f64(fres);
        auto const speed_limit = f64(Settings::SpeedLimit()) * 0.01;
        return u64(ticks / speed_limit);
    }
    return fres;
}

u64 CoreTiming::GetGPUTicks() const {
    return is_multicore
        ? Common::g_wall_clock.GetGPUTick()
        : Common::WallClock::CPUTickToGPUTick(cpu_ticks);
}

std::optional<s64> CoreTiming::Advance() {
    std::scoped_lock lock{advance_lock, basic_lock};
    // Scanning: events scheduled from here on are seen below, so they need no wake-up.
    planned_wake = std::numeric_limits<s64>::min();
    global_timer = GetGlobalTimeNs().count();

    while (!event_queue.empty() && event_queue.top().time <= global_timer) {
        const Event& evt = event_queue.top();

        const auto event_type{evt.type.lock()};
        if (!event_type) {
            // Its type is gone (destroyed without UnscheduleEvent): drop it. Left at the top, the
            // loop would spin here for good, holding the lock every ScheduleEvent needs.
            event_queue.pop();
            global_timer = GetGlobalTimeNs().count();
            continue;
        }
        {
            const auto evt_time = evt.time;
            const auto evt_sequence_num = event_type->sequence_number;

            if (evt.reschedule_time == 0) {
                event_queue.pop();

                basic_lock.unlock();

                const s64 begin = GetGlobalTimeNs().count();
                event_type->callback(evt_time, std::chrono::nanoseconds{begin - evt_time});
                if (trace) {
                    trace->NoteCallback(event_type->name, begin - evt_time,
                                        GetGlobalTimeNs().count() - begin);
                }

                basic_lock.lock();
            } else {
                basic_lock.unlock();

                const s64 begin = GetGlobalTimeNs().count();
                const auto new_schedule_time{
                    event_type->callback(evt_time, std::chrono::nanoseconds{begin - evt_time})};
                if (trace) {
                    trace->NoteCallback(event_type->name, begin - evt_time,
                                        GetGlobalTimeNs().count() - begin);
                }

                basic_lock.lock();

                if (evt_sequence_num != event_type->sequence_number) {
                    // Heap handle is invalidated after external modification.
                    continue;
                }

                const auto next_schedule_time{new_schedule_time.has_value()
                                                  ? new_schedule_time.value().count()
                                                  : evt.reschedule_time};

                // If this event was scheduled into a pause, its time now is going to be way
                // behind. Re-set this event to continue from the end of the pause.
                auto next_time{evt.time + next_schedule_time};
                if (evt.time < pause_end_time) {
                    next_time = pause_end_time + next_schedule_time;
                }

                // The callback may have changed whether the event is relaxed (SetRelaxed).
                event_queue.update(evt.handle,
                                   Event{next_time, event_fifo_id++, evt.type, next_schedule_time,
                                         evt.handle, event_type->IsRelaxed()});
            }
        }

        global_timer = GetGlobalTimeNs().count();
    }

    if (!event_queue.empty()) {
        os_wake_guard = os_wake_guard_override >= 0 ? os_wake_guard_override : OsWakeGuardNs();
        s64 os_wake = std::numeric_limits<s64>::max();
        const EventType* os_wake_type{};
        for (const Event& queued : event_queue) {
            const s64 wake = Common::OsWakeTime(queued.time, queued.relaxed, os_wake_guard);
            if (wake < os_wake) {
                os_wake = wake;
                if (trace) {
                    const auto type = queued.type.lock();
                    os_wake_type = type.get();
                }
            }
        }
        const s64 top = event_queue.top().time;
        // Decided here, under the lock, so ScheduleEvent compares against what the thread does.
        next_wait.blocks = os_wake > GetGlobalTimeNs().count();
        next_wait.until = next_wait.blocks ? os_wake : top;
        if (trace) {
            next_wait.name = os_wake_type != nullptr ? os_wake_type->name : std::string{"?"};
        }
        planned_wake = next_wait.until;
        return top;
    } else {
        planned_wake = std::numeric_limits<s64>::max();
        return std::nullopt;
    }
}

void CoreTiming::Reset() {
    paused = true;
    // Request the stop before waking the thread: woken first, it could take the wake-up, see no
    // stop request yet, and go back to waiting on pause_event for good (the join below then hung
    // the frontend at boot).
    if (timer_thread.joinable()) {
        timer_thread.request_stop();
    }
    pause_event.Set();
    event.Set();
    if (timer_thread.joinable()) {
        timer_thread.join();
    }
    has_started = false;
}

/// @brief Returns current time in nanoseconds.
std::chrono::nanoseconds CoreTiming::GetGlobalTimeNs() const noexcept {
    return is_multicore
        ? Common::g_wall_clock.GetTimeNS()
        : std::chrono::nanoseconds{Common::WallClock::CPUTickToNS(cpu_ticks)};
}

/// @brief Returns current time in microseconds.
std::chrono::microseconds CoreTiming::GetGlobalTimeUs() const noexcept {
    return is_multicore
        ? Common::g_wall_clock.GetTimeUS()
        : std::chrono::microseconds{Common::WallClock::CPUTickToUS(cpu_ticks)};
}

#ifdef _WIN32
void CoreTiming::SetTimerResolutionNs(std::chrono::nanoseconds ns) {
    timer_resolution_ns = ns.count();
}
#endif

} // namespace Core::Timing
