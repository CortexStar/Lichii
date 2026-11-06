// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include "common/logging.h"
#include "common/game_presets.h"
#include "core/arm/guest_hooks.h"
#include "core/memory.h"

namespace Core::GuestHooks {

namespace {

u32 RacerMask() {
    static const u32 mask = [] {
        const char* value = std::getenv("EDEN_MKW_RACERS");
        if (value == nullptr) {
            return 0u;
        }
        const int racers = std::atoi(value);
        if (racers < 1 || racers > 24) {
            LOG_WARNING(Core_ARM, "EDEN_MKW_RACERS={} ignored (1-24)", value);
            return 0u;
        }
        return (1u << racers) - 1u;
    }();
    return mask;
}

void ReadyRacePlayerBitmap(NativeCall& call) {
    const u64 address = call.registers.Get(8);
    const u32 bitmap = call.memory.Read32(address);
    const u32 masked = bitmap & RacerMask();
    call.registers.Set(8, masked);
    LOG_INFO(Core_ARM, "Mario Kart World: race slots {:#x} -> {:#x}", bitmap, masked);
}

} // namespace

void RegisterMarioKartWorldHooks() {
    if (RacerMask() == 0) {
        return;
    }
    Register(Hook{
        .name = "mkw-racer-count",
        .program_id = Settings::MarioKartWorldProgramId,
        .module = "main",
        .offset = 0x6573ec,
        .original =
            {
                0xB9400108,
                0xB81FC3A8,
                0xF9400668,
                0xF9400508,
                0xF940E502,
                0x945FB2D1,
            },
        .native = ReadyRacePlayerBitmap,
    });
    Register(Hook{
        .name = "mkw-racer-count-info",
        .program_id = Settings::MarioKartWorldProgramId,
        .module = "main",
        .offset = 0x1a5e2cc,
        .original = {0xB9400108, 0xB9001BA8, 0x97B9BD8A, 0xAA1503E0},
        .native = ReadyRacePlayerBitmap,
    });
    Register(Hook{
        .name = "mkw-racer-count-race",
        .program_id = Settings::MarioKartWorldProgramId,
        .module = "main",
        .offset = 0x1a5e314,
        .original = {0xB9400108, 0xB9002AC8, 0x97AFE601, 0x910063A1},
        .native = ReadyRacePlayerBitmap,
    });
    Register(Hook{
        .name = "mkw-racer-count-log",
        .program_id = Settings::MarioKartWorldProgramId,
        .module = "main",
        .offset = 0x1a5e370,
        .original = {0x2A160108, 0xB9001FA8, 0x97B9BD64},
        .native =
            [](NativeCall& call) {
                const u32 w8 = static_cast<u32>(call.registers.Get(8));
                const u32 w22 = static_cast<u32>(call.registers.Get(22));
                call.registers.Set(8, w8 | w22);
                LOG_INFO(Core_ARM, "Mario Kart World: race setup slots {:#x} | {:#x} = {:#x}", w8,
                         w22, w8 | w22);
            },
    });
}

} // namespace Core::GuestHooks
