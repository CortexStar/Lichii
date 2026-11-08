// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "common/container/unordered_map.h"

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"

#include "core/hle/service/vi/vsync_manager.h"

namespace Core {
class System;
}

namespace Core::Timing {
struct EventType;
}

namespace Service {
class Event;
}

namespace Service::VI {

/**
 * Scale of the Switch 1 compositor's vsync period (ScreenComposition).
 *
 * Unlocked Switch 1 titles present through this compositor, so with the speed limit off it ticks
 * 100x faster to let them run free. Switch 2 titles present and pace through nx2:vi2 instead, so
 * for them it stays at the display rate: at 1/100 of the period the timer thread spent 82% of
 * its time in precise waits for 6,000 composition ticks a second that composed nothing.
 */
[[nodiscard]] constexpr float ScreenCompositionSpeedScale(bool multi_core, bool speed_limit_on,
                                                          float speed_limit_percent,
                                                          bool switch2) {
    if (!multi_core) {
        return 1.f;
    }
    if (speed_limit_on) {
        return 100.f / speed_limit_percent;
    }
    return switch2 ? 1.f : 0.01f;
}

class Container;
class DisplayList;

class Conductor {
public:
    explicit Conductor(Core::System& system, Container& container, DisplayList& displays);
    ~Conductor();

    void LinkVsyncEvent(u64 display_id, Event* event);
    void UnlinkVsyncEvent(u64 display_id, Event* event);

private:
    void ProcessVsync();
    void VsyncThread(std::stop_token token);
    s64 GetNextTicks() const;
    s64 GetFramePeriodNs() const;

private:
    Core::System& m_system;
    Container& m_container;
    ::Common::unordered_map<u64, VsyncManager> m_vsync_managers;
    std::shared_ptr<Core::Timing::EventType> m_event;
    Common::Event m_signal;
    std::jthread m_thread;

private:
    s32 m_swap_interval = 1;
    f32 m_compose_speed_scale = 1.0f;
};

} // namespace Service::VI
