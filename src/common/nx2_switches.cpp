// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EXPERIMENT (do not merge): see nx2_switches.h.

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "common/logging.h"
#include "common/nx2_switches.h"

namespace Common::Nx2Switches {
namespace {

enum class Rule {
    One,     ///< set when the value starts with '1'
    Zero,    ///< set when the value starts with '0'
    IsZero,  ///< set when the value is exactly "0"
    IsAll,   ///< set when the value is exactly "all"
    Present, ///< set when the variable exists at all
    Names,   ///< a GuestCpuOffBits mask of the names the value contains
};

struct Info {
    std::string_view name;
    Rule rule;
};

constexpr size_t COUNT = static_cast<size_t>(Switch::Count);

constexpr std::array<Info, COUNT> INFOS{{
    {"EDEN_NX2_EXP_NO_PARK", Rule::One},
    {"EDEN_NX2_EXP_NO_FENCE_QUIET", Rule::One},
    {"EDEN_NX2_EXP_FENCE_FLUSH_TWICE", Rule::One},
    {"EDEN_NX2_EXP_COLD_ALWAYS", Rule::One},
    {"EDEN_NX2_EXP_NO_EARLY_READ", Rule::One},
    {"EDEN_NX2_EXP_NO_UPLOAD_BATCH", Rule::One},
    {"EDEN_NX2_GUEST_CPU_OFF", Rule::Names},
    {"EDEN_NX2_EXP_DISPATCH_BARRIERS", Rule::IsAll},
    {"EDEN_NX2_DYNSTATE_BATCH", Rule::IsZero},
    {"EDEN_NX2_PRESENT_CADENCE", Rule::Zero},
    {"EDEN_NX2_EXP_FG_HUDLESS", Rule::Zero},
    {"EDEN_NX2_EXP_HEAVY_DISPATCH", Rule::One},
    {"EDEN_NX2_EXP_PRESENT_FILTERS", Rule::One},
    {"EDEN_NX2_EXP_NO_CLAIM", Rule::Present},
    {"EDEN_NX2_EXP_NO_BUILDER_BOOST", Rule::Present},
    {"EDEN_NX2_EXP_EVICT_9000", Rule::Present},
    {"EDEN_NX2_EXP_NO_GPL_RELINK", Rule::Present},
}};

constexpr std::array<std::pair<std::string_view, u32>, 6> GUEST_CPU_NAMES{{
    {"cntpct", Cntpct},
    {"spin", Spin},
    {"idle", Idle},
    {"arb", Arb},
    {"hostfn", Hostfn},
    {"svcwait", Svcwait},
}};

struct Registry {
    Registry() {
        for (size_t i = 0; i < COUNT; ++i) {
            const char* const value = std::getenv(std::string{INFOS[i].name}.c_str());
            if (value != nullptr) {
                text[i] = std::string{value};
            }
            const u32 parsed =
                Parse(static_cast<Switch>(i),
                      value != nullptr ? std::optional<std::string_view>{value} : std::nullopt);
            launch[i] = parsed;
            active[i].store(parsed, std::memory_order_relaxed);
            requested[i] = parsed;
        }
    }

    std::array<std::atomic<u32>, COUNT> active{};
    std::array<u32, COUNT> launch{};
    /// Guarded by mutex.
    std::array<u32, COUNT> requested{};
    std::array<std::optional<std::string>, COUNT> text{};
    std::mutex mutex;
    std::atomic<bool> pending{};
};

Registry& Instance() {
    static Registry registry;
    return registry;
}

size_t Index(Switch which) {
    return static_cast<size_t>(which);
}

} // Anonymous namespace

u32 Get(Switch which) noexcept {
    return Instance().active[Index(which)].load(std::memory_order_relaxed);
}

u32 AtLaunch(Switch which) noexcept {
    return Instance().launch[Index(which)];
}

std::string_view Name(Switch which) noexcept {
    return INFOS[Index(which)].name;
}

std::optional<Switch> Find(std::string_view name) noexcept {
    for (size_t i = 0; i < COUNT; ++i) {
        if (INFOS[i].name == name) {
            return static_cast<Switch>(i);
        }
    }
    return std::nullopt;
}

u32 Parse(Switch which, std::optional<std::string_view> text) {
    const Rule rule = INFOS[Index(which)].rule;
    if (!text) {
        return 0;
    }
    const std::string_view value = *text;
    switch (rule) {
    case Rule::One:
        return !value.empty() && value[0] == '1' ? 1 : 0;
    case Rule::Zero:
        return !value.empty() && value[0] == '0' ? 1 : 0;
    case Rule::IsZero:
        return value == "0" ? 1 : 0;
    case Rule::IsAll:
        return value == "all" ? 1 : 0;
    case Rule::Present:
        return 1;
    case Rule::Names: {
        u32 mask = 0;
        for (const auto& [name, bit] : GUEST_CPU_NAMES) {
            if (value.find(name) != std::string_view::npos) {
                mask |= bit;
            }
        }
        return mask;
    }
    }
    return 0;
}

u32 Request(Switch which, std::optional<std::string_view> text) {
    auto& registry = Instance();
    const u32 value = Parse(which, text);
    std::scoped_lock lock{registry.mutex};
    registry.requested[Index(which)] = value;
    registry.text[Index(which)] =
        text ? std::optional<std::string>{std::string{*text}} : std::nullopt;
    registry.pending.store(true, std::memory_order_release);
    return value;
}

std::optional<std::string> Text(Switch which) {
    auto& registry = Instance();
    std::scoped_lock lock{registry.mutex};
    return registry.text[Index(which)];
}

std::vector<Change> Latch(u64 frame) {
    auto& registry = Instance();
    if (!registry.pending.load(std::memory_order_acquire)) {
        return {};
    }
    std::vector<Change> changes;
    std::scoped_lock lock{registry.mutex};
    registry.pending.store(false, std::memory_order_relaxed);
    for (size_t i = 0; i < COUNT; ++i) {
        const u32 from = registry.active[i].load(std::memory_order_relaxed);
        const u32 to = registry.requested[i];
        if (from == to) {
            continue;
        }
        registry.active[i].store(to, std::memory_order_relaxed);
        changes.push_back({static_cast<Switch>(i), from, to});
        LOG_INFO(Debug, "switch {} = {} ({} -> {}) at frame {}", INFOS[i].name,
                 registry.text[i] ? *registry.text[i] : std::string{"(unset)"}, from, to, frame);
    }
    return changes;
}

} // namespace Common::Nx2Switches
