// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <initializer_list>
#include <map>
#include <optional>
#include <tuple>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"
#include <boost/container/static_vector.hpp>

#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/atomic_fallback.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::X64 {

/// UserConfig::host_call_return in effect: it needs rsp-free blocks (no %rbp frame, no cycle
/// counter on the stack).
inline bool HostCallReturn(const A64::UserConfig& conf) {
    return conf.host_call_return && conf.omit_block_frame && !conf.enable_cycle_counting;
}

struct A64EmitContext final : public EmitContext {
    A64EmitContext(const A64::UserConfig& conf, RegAlloc& reg_alloc, IR::Block& block, boost::container::stable_vector<Xbyak::Label>& shared_labels);

    A64::LocationDescriptor Location() const;
    bool IsSingleStep() const;
    FP::FPCR FPCR(bool fpcr_controlled = true) const override;

    bool HasOptimization(OptimizationFlag flag) const override {
        return conf.HasOptimization(flag);
    }

    const A64::UserConfig& conf;
};

class A64EmitX64 final : public EmitX64 {
public:
    A64EmitX64(BlockOfCode& code, A64::UserConfig conf, A64::Jit* jit_interface);
    ~A64EmitX64() override;

    /// Emit host machine code for a basic block with intermediate representation `block`.
    /// @note block is modified.
    BlockDescriptor Emit(IR::Block& block) noexcept;
    /// Emit's work; may throw Xbyak::Error, which Emit reports with the block's PC.
    BlockDescriptor EmitBlock(IR::Block& block);

    void ClearCache() override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

//protected:
    struct FastDispatchEntry {
        u64 location_descriptor = 0xFFFF'FFFF'FFFF'FFFFull;
        const void* code_ptr = nullptr;
    };
    static_assert(sizeof(FastDispatchEntry) == 0x10);
    static constexpr size_t default_fast_dispatch_table_bits = 20;
    /// Entries per set when conf.fast_dispatch_set_associative (one 64-byte cache line).
    static constexpr size_t fast_dispatch_ways = 4;

    void ClearFastDispatchTable();
    void GenMemory128Accessors();
    void GenFastmemFallbacks();
    void GenTerminalHandlers();

    // Microinstruction emitters
    void EmitPushRSB(EmitContext& ctx, IR::Inst* inst);
#define OPCODE(...)
#define A32OPC(...)
#define A64OPC(name, type, ...) void EmitA64##name(A64EmitContext& ctx, IR::Inst* inst);
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

    // Helpers
    std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const override;

    // Fastmem information
    using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;
    /// Where a fastmem access that faults goes: code addresses are offsets in the code space.
    struct FastmemPatchInfo {
        u32 rip;
        u32 resume_rip;
        u32 callback;
        u32 inst_name : 31;
        u32 recompile : 1;
        IR::LocationDescriptor location;
    };
    std::optional<DoNotFastmemMarker> ShouldFastmem(A64EmitContext& ctx, IR::Inst* inst) const;
    void AddFastmemPatch(CodePtr location, CodePtr resume_location, void (*callback)(), const DoNotFastmemMarker& marker, bool recompile);
    FakeCall FastmemCallback(u64 rip);

    // Memory access helpers
    void EmitCheckMemoryAbort(A64EmitContext& ctx, IR::Inst* inst, Xbyak::Label* end = nullptr);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryRead(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryWrite(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemoryInline(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemoryInline(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize>
    void EmitAtomicRmwMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize>
    void EmitAtomicCasMemory(A64EmitContext& ctx, IR::Inst* inst);
    using AtomicFallbackContext = X64::AtomicFallbackContext;
    using AtomicFallbackFn = u64 (*)(const AtomicFallbackContext& context, u64 vaddr, u64 a, u64 b);
    void EmitAtomicFallbackStub(Xbyak::Reg64 result, std::initializer_list<Xbyak::Reg64> sources, std::optional<u32> param4, AtomicFallbackFn fallback);

    // Terminal instruction emitters
    bool EmitLeafTerminal(IR::Term::LeafTerminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;
    bool EmitTerminal(IR::Term::Terminal const& terminal, IR::LocationDescriptor initial_location, bool is_single_step) noexcept override;

    // Patching
    void Unpatch(const IR::LocationDescriptor& target_desc) override;
    void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) override;

    /// The JIT state's halt reasons, for the atomics' callback paths (AtomicFallbackContext).
    void SetAtomicFallbackHaltReason(const volatile u32* halt_reason) {
        atomic_fallback.halt_reason = halt_reason;
    }

//data
    const A64::UserConfig conf;
    AtomicFallbackContext atomic_fallback{&conf, nullptr};
    RegAlloc reg_alloc; //reusable reg alloc
    BlockRangeInformation<u64> block_ranges;
    std::vector<FastDispatchEntry> fast_dispatch_table;
    /// Byte offset mask of a set (or entry) in fast_dispatch_table.
    u32 fast_dispatch_table_mask = 0;
    size_t fast_dispatch_set_size = 1;
    /// EmitA64SetCheckBit left the check bit in ZF for the CheckBit terminal.
    bool check_bit_in_zf = false;

    /// A UserCallbacks member call; with core_independent_code `this` comes from the JIT state.
    /// Every core's callbacks object has the same type, so the devirtualized target is the same.
    template<auto mfp>
    ArgCallback UserCallback() const {
        const ArgCallback callback = Devirtualize<mfp>(conf.callbacks);
        if (!conf.core_independent_code) {
            return callback;
        }
        ASSERT(callback.Arg() == std::bit_cast<u64>(conf.callbacks));
        return callback.WithArgFromJitState(s32(offsetof(A64JitState, callbacks)));
    }
    /// reg = a per-Jit pointer: from the JIT state with core_independent_code, else `value`.
    void EmitLoadJitPointer(Xbyak::Reg64 reg, size_t jit_state_offset, u64 value) const;
    /// reg = &conf (the Jit's UserConfig).
    void EmitLoadUserConfig(Xbyak::Reg64 reg) const;
    /// reg = the AtomicFallbackContext the LSE fallbacks take.
    void EmitLoadAtomicFallback(Xbyak::Reg64 reg) const;
    /// With core_independent_code: where this processor's exclusive slot pointer is.
    std::optional<s32> OwnExclusiveSlotOffset() const;
    /// reg = this processor's exclusive-monitor address (or value) slot.
    void EmitLoadExclusiveSlot(Xbyak::Reg64 reg, bool value_slot) const;
    /// host_call_return: the guest return address of the block's BL/BLR, pushed as a host call
    /// frame right before the terminal.
    std::optional<IR::LocationDescriptor> call_return_target;
    void EmitHostCall(const IR::LocationDescriptor& return_target);
    /// Where the check bit lives: the JIT state with host_call_return, else the stack.
    Xbyak::Address CheckBitAddress() const;

    // Compact block exits (conf.compact_block_exits): one shared unlinked-exit stub per target
    // descriptor, created while a block that links to it is emitted, and kept until ClearCache.
    ::Common::unordered_map<u64, CodePtr> exit_stubs;
    std::vector<std::pair<u64, Xbyak::Label*>> pending_exit_stubs;
    bool emitting_block = false;
    Xbyak::Label* PendingExitStub(const IR::LocationDescriptor& target);
    void EnsureExitStub(const IR::LocationDescriptor& target);
    void FlushPendingExitStubs();
    /// Leaves the block for `target` without linking: stores the PC and returns to the dispatcher.
    void EmitUnlinkedExit(const IR::LocationDescriptor& target);
    std::vector<FastmemPatchInfo> fastmem_patch_info;  // sorted by rip
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> read_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> write_fallbacks;
    ::Common::unordered_map<std::tuple<bool, size_t, int, int>, void (*)()> exclusive_write_fallbacks;
    ::Common::unordered_set<DoNotFastmemMarker> do_not_fastmem;
    boost::container::stable_vector<Xbyak::Label> shared_labels;
    const void* terminal_handler_pop_rsb_hint = nullptr;
    const void* terminal_handler_fast_dispatch_hint = nullptr;
    /// indirect_branch_cache: the fast dispatcher that also writes the site's entry (at rbp).
    const void* terminal_handler_fast_dispatch_fill = nullptr;
    struct IndirectBranchCacheEntry {
        u64 pc;
        u64 code_ptr;
        u8* site;    // the site's PC load, overwritten with a jump to the fast dispatcher once megamorphic
        u64 misses;  // refills so far
    };
    static constexpr u64 indirect_branch_cache_max_misses = 16;
    static constexpr size_t indirect_branch_cache_entries = 32768;
    IndirectBranchCacheEntry* indirect_branch_cache = nullptr;  // in the code space, own pages
    size_t indirect_branch_cache_used = 0;
    /// Incremented by every whole-cache clear (the fill handler checks it around a compile).
    u64 indirect_branch_generation = 0;
    /// The block being emitted writes FPCR (A64SetFPCR).
    bool block_writes_fpcr = false;
    void ClearIndirectBranchCache();
    /// Indirect jump to the guest PC in A64JitState::pc through a new cache entry; false when
    /// none is left (the caller then takes the fast dispatcher).
    bool EmitIndirectBranchCache();
    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64) = nullptr;
    A64::Jit* jit_interface = nullptr;
    void (*memory_read_128)() = nullptr;
    void (*memory_write_128)() = nullptr;
    void (*memory_exclusive_write_128)() = nullptr;
};

}  // namespace Dynarmic::Backend::X64
