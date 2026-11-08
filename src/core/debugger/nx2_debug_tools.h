// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/common_types.h"
#include "common/nx2_switches.h"
#include "hid_core/frontend/emulated_controller.h"

namespace Core {
class System;
}

namespace Core::Memory {
class Memory;
}

namespace Core::Nx2Debug {

class Server;
struct Endpoint;
struct ToolState;

/// The debug server of one boot (docs/nx2/debug-mcp.md): started when a game has loaded, stopped
/// first when it shuts down, so the tools never see a half-built or half-destroyed system.
class Service {
public:
    explicit Service(System& system, std::string title, u64 program_id, const Endpoint& endpoint,
                     std::string dump_directory);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

private:
    std::shared_ptr<ToolState> state;
    std::unique_ptr<Server> server;
};

/// A reject_shaders hash: exactly 16 hex digits, optionally after 0x. Throws std::runtime_error.
[[nodiscard]] u64 ParseShaderHash(std::string_view text);

/// search_memory's pattern: hex digit pairs, optionally after 0x, spaces allowed. Throws
/// std::runtime_error.
[[nodiscard]] std::vector<u8> ParseHexBytes(std::string_view text);

/// read_memory's formats: {"hex": "..."} or {"u32"|"u64"|"f32": [...]}; u64 values are hex strings
/// and non-finite floats are "nan", "inf" or "-inf". Throws std::runtime_error when the format is
/// unknown or the size is not a multiple of its element.
[[nodiscard]] nlohmann::json FormatMemory(std::span<const u8> bytes, std::string_view as);

/// Copies guest memory from a thread that is not a guest CPU core (docs/nx2/debug-mcp.md, "Guest
/// and GPU memory"). Returns the first address that is not mapped; out is then partly written.
[[nodiscard]] std::optional<u64> ReadGuestMemory(Memory::Memory& memory, u64 address,
                                                 std::span<u8> out);

struct GuestSearch {
    std::vector<u64> hits;
    /// Where to resume: the first address no match was looked for at; end when complete.
    u64 next;
    bool complete;
    /// Mapped bytes read.
    u64 scanned;
};

/// Finds the pattern in the mapped pages of [start, end), as ReadGuestMemory reads them. Stops at
/// max_hits or the deadline. Matches may span pages, but not unmapped gaps.
[[nodiscard]] GuestSearch SearchGuestMemory(Memory::Memory& memory, std::span<const u8> pattern,
                                            u64 start, u64 end, size_t max_hits,
                                            std::chrono::steady_clock::time_point deadline);

/// One step of input or input_sequence: what to hold, for how many presented frames or for how many
/// milliseconds of wall time. Exactly one of frames and ms is non-zero.
struct InputStep {
    HID::InjectedInput input;
    u64 frames;
    u64 ms;
};

/// {buttons?, left_stick?, right_stick?, frames | ms}. Sticks are [x, y] in -1..1. Throws
/// std::runtime_error on unknown keys, buttons or out-of-range values, and unless exactly one of
/// frames and ms is given.
[[nodiscard]] InputStep ParseInputStep(const nlohmann::json& step);

/// EXPERIMENT (do not merge): a `switch` request, {name, value?}. A missing or null value unsets
/// the variable; a number counts as its decimal text.
struct SwitchRequest {
    Common::Nx2Switches::Switch which;
    std::optional<std::string> value;
};

/// Throws std::runtime_error on a name that is no runtime switch, naming the ones that are.
[[nodiscard]] SwitchRequest ParseSwitchRequest(const nlohmann::json& params);

/// The steps of one input or input_sequence call, run back to back from present count start on.
/// A frames step ends after that many presents, an ms step after that much wall time. Callers
/// pass the present count and a steady clock in nanoseconds; the script moves to the next step on
/// the first call past a step's end. Safe to call from several threads.
class InputScript {
public:
    InputScript(std::vector<InputStep> steps, u64 start);

    /// The input of the active step; nothing before start and from the end of the last step on.
    [[nodiscard]] std::optional<HID::InjectedInput> At(u64 present, u64 now_ns);

    /// Index of the active step: 0 before start, the step count once all are done.
    [[nodiscard]] size_t Step(u64 present, u64 now_ns);

private:
    void Advance(u64 present, u64 now_ns);

    std::mutex mutex;
    const std::vector<InputStep> steps;
    const u64 start;
    bool started{};
    size_t index{};
    /// Present count from which the active step's frames are counted.
    u64 step_present{};
    /// Steady time at which the active step began. Consecutive ms steps chain from each other's
    /// end, so a polling delay does not add up over a sequence.
    u64 step_ns{};
};

/// How long the present count has not moved: an input call waits for frames as long as the game
/// still presents, however slowly, and gives up only on a game that stopped presenting.
class PresentStall {
public:
    PresentStall(u64 present, u64 now_ns);

    /// Nanoseconds since the count last changed (0 on the call that sees it change).
    [[nodiscard]] u64 StalledNs(u64 present, u64 now_ns);

private:
    u64 last_present;
    u64 last_ns;
};

/// Guest-present intervals within [begin_ns, end_ns], using only retained ring timestamps.
[[nodiscard]] nlohmann::json ReadFrameTimes(u64 before, u64 after, u64 begin_ns, u64 end_ns,
                                          double refresh_hz);

/// The service when EDEN_NX2_DEBUG_SERVER is set and it can listen, otherwise null.
[[nodiscard]] std::unique_ptr<Service> MaybeStartService(System& system, std::string title,
                                                         u64 program_id);

} // namespace Core::Nx2Debug
