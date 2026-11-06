// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Kernel {
class KProcess;
}

namespace Core::Memory {
class Memory;
}

namespace Core::GuestHooks {

class Registers {
public:
    virtual ~Registers() = default;
    virtual u64 Get(u32 index) const = 0;
    virtual void Set(u32 index, u64 value) = 0;
    virtual u64 GetSP() const = 0;
    virtual void SetSP(u64 value) = 0;
    virtual u64 GetPC() const = 0;
    virtual void SetPC(u64 value) = 0;
};

struct NativeCall {
    Registers& registers;
    Core::Memory::Memory& memory;
    u64 module_base;
    u64 hook_address;
};

using NativeHandler = void (*)(NativeCall& call);

struct Hook {
    std::string name;
    u64 program_id{};
    std::string module;
    u64 offset{};
    std::vector<u32> original;
    std::vector<u32> replacement;
    NativeHandler native{};
};

inline constexpr u32 NativeBreakpoint = 0xD4200000u | (0x4855u << 5);

bool Register(Hook hook);

size_t OnModuleLoaded(const Kernel::KProcess* process, u64 program_id, std::string_view module,
                      u64 base, std::span<const u8> image);

void OnProcessExit(const Kernel::KProcess* process);

std::optional<u32> InstructionFor(const Kernel::KProcess* process, u64 vaddr);

bool RunNative(const Kernel::KProcess* process, u64 pc, Registers& registers,
               Core::Memory::Memory& memory);

void RegisterMarioKartWorldHooks();

void ResetForTesting();

} // namespace Core::GuestHooks
