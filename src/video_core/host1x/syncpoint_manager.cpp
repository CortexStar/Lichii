// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/logging.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    std::scoped_lock lk(guard);
    if (syncpoint.load(std::memory_order_relaxed) >= expected_value) {
        action();
        return {};
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value >= expected_value) {
            break;
        }
        ++it;
    }
    return action_storage.emplace(it, expected_value, ++last_action_id, std::move(action))->id;
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        const ActionHandle& handle) {
    if (handle == 0) {
        return;
    }
    std::scoped_lock lk(guard);

    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it->id == handle) {
            action_storage.erase(it);
            return;
        }
    }
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, const ActionHandle& handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        LOG_ERROR(HW_GPU, "Ignoring increment of invalid syncpoint {:#x}", syncpoint_id);
        return;
    }
    Increment(syncpoint_id, syncpoints_guest[syncpoint_id], wait_guest_cv,
              guest_action_storage[syncpoint_id], true);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (syncpoint_id >= NUM_MAX_SYNCPOINTS) {
        return;
    }
    Increment(syncpoint_id, syncpoints_host[syncpoint_id], wait_host_cv,
              host_action_storage[syncpoint_id], false);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}

SyncpointManager::ActionHandle SyncpointManager::AddGuestIncrementObserver(
    IncrementObserver&& observer) {
    std::scoped_lock lk(guard);
    return guest_increment_observers.emplace_back(++last_action_id, std::move(observer)).first;
}

void SyncpointManager::RemoveGuestIncrementObserver(const ActionHandle& handle) {
    std::scoped_lock lk(guard);
    std::erase_if(guest_increment_observers,
                  [handle](const auto& observer) { return observer.first == handle; });
}

void SyncpointManager::Increment(u32 syncpoint_id, std::atomic<u32>& syncpoint,
                                 std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage, bool guest) {
    std::scoped_lock lk(guard);
    // Incremented under the lock, so observers get each syncpoint's values in order.
    const u32 new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};
    if (guest) {
        for (const auto& [handle, observer] : guest_increment_observers) {
            observer(syncpoint_id, new_value);
        }
    }
    auto it = action_storage.begin();
    while (it != action_storage.end()) {
        if (it->expected_value > new_value) {
            break;
        }
        it->action();
        it = action_storage.erase(it);
    }
    wait_cv.notify_all();
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    wait_cv.wait(lk, pred);
}

} // namespace Host1x

} // namespace Tegra
