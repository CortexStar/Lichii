// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>

#include "common/logging.h"
#include "core/arm/guest_hooks.h"

namespace Core::GuestHooks {

namespace {

struct Active {
    const Kernel::KProcess* process;
    u64 address;
    u64 module_base;
    const Hook* hook;
};

struct State {
    std::shared_mutex mutex;
    std::vector<std::unique_ptr<Hook>> registered;
    std::vector<Active> active;
    std::atomic<bool> any_active{false};
};

State& GetState() {
    static State state;
    return state;
}

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_HOOKS");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

const Active* Find(const State& state, const Kernel::KProcess* process, u64 vaddr) {
    auto it = std::upper_bound(state.active.begin(), state.active.end(), vaddr,
                               [](u64 address, const Active& a) { return address < a.address; });
    while (it != state.active.begin()) {
        --it;
        const u64 end = it->address + it->hook->original.size() * sizeof(u32);
        if (it->process == process && vaddr < end) {
            return &*it;
        }
        if (vaddr - it->address > 0x10000) {
            break;
        }
    }
    return nullptr;
}

} // namespace

bool Register(Hook hook) {
    const bool native = hook.native != nullptr;
    if (hook.original.empty() || hook.offset % sizeof(u32) != 0 ||
        (native ? !hook.replacement.empty()
                : hook.replacement.size() != hook.original.size())) {
        LOG_ERROR(Core_ARM, "Guest hook {} is malformed and was not registered", hook.name);
        return false;
    }
    auto& state = GetState();
    std::unique_lock lock{state.mutex};
    state.registered.push_back(std::make_unique<Hook>(std::move(hook)));
    return true;
}

size_t OnModuleLoaded(const Kernel::KProcess* process, u64 program_id, std::string_view module,
                      u64 base, std::span<const u8> image) {
    if (!Enabled()) {
        return 0;
    }
    static std::once_flag builtins;
    std::call_once(builtins, [] { RegisterMarioKartWorldHooks(); });
    auto& state = GetState();
    std::unique_lock lock{state.mutex};
    size_t activated = 0;
    for (const auto& hook : state.registered) {
        if (hook->program_id != program_id || hook->module != module) {
            continue;
        }
        const size_t bytes = hook->original.size() * sizeof(u32);
        bool matches = hook->offset + bytes <= image.size();
        for (size_t i = 0; matches && i < hook->original.size(); ++i) {
            u32 word;
            std::memcpy(&word, image.data() + hook->offset + i * sizeof(u32), sizeof(word));
            matches = word == hook->original[i];
        }
        if (!matches) {
            LOG_WARNING(Core_ARM, "Guest hook {} not applied: {}+{:#x} doesn't hold the expected "
                                  "code (another version?)",
                        hook->name, module, hook->offset);
            continue;
        }
        std::erase_if(state.active, [&](const Active& a) {
            return a.process == process && a.hook == hook.get();
        });
        state.active.push_back({process, base + hook->offset, base, hook.get()});
        ++activated;
        LOG_INFO(Core_ARM, "Guest hook {} active at {}+{:#x} ({} words, {})", hook->name, module,
                 hook->offset, hook->original.size(), hook->native ? "native" : "replace");
    }
    std::ranges::sort(state.active, {}, &Active::address);
    state.any_active.store(!state.active.empty(), std::memory_order_release);
    return activated;
}

void OnProcessExit(const Kernel::KProcess* process) {
    auto& state = GetState();
    std::unique_lock lock{state.mutex};
    std::erase_if(state.active, [&](const Active& a) { return a.process == process; });
    state.any_active.store(!state.active.empty(), std::memory_order_release);
}

std::optional<u32> InstructionFor(const Kernel::KProcess* process, u64 vaddr) {
    auto& state = GetState();
    if (!state.any_active.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::shared_lock lock{state.mutex};
    const Active* const active = Find(state, process, vaddr);
    if (!active) {
        return std::nullopt;
    }
    const size_t index = (vaddr - active->address) / sizeof(u32);
    if (active->hook->native) {
        return index == 0 ? std::optional<u32>{NativeBreakpoint} : std::nullopt;
    }
    return active->hook->replacement[index];
}

bool RunNative(const Kernel::KProcess* process, u64 pc, Registers& registers,
               Core::Memory::Memory& memory) {
    auto& state = GetState();
    if (!state.any_active.load(std::memory_order_acquire)) {
        return false;
    }
    NativeHandler handler;
    u64 module_base;
    {
        std::shared_lock lock{state.mutex};
        const Active* const active = Find(state, process, pc);
        if (!active || active->address != pc || !active->hook->native) {
            return false;
        }
        handler = active->hook->native;
        module_base = active->module_base;
    }
    NativeCall call{registers, memory, module_base, pc};
    handler(call);
    return true;
}

void ResetForTesting() {
    auto& state = GetState();
    std::unique_lock lock{state.mutex};
    state.active.clear();
    state.registered.clear();
    state.any_active.store(false, std::memory_order_release);
}

} // namespace Core::GuestHooks
