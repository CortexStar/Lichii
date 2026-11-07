// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include "common/logging.h"
#include "common/scratch_buffer.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc/svc_nx2_zstd.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"

namespace Kernel::Svc {
namespace {

// Ocarina of Time (main build E0F3CCFA...): the asset streaming jobs decompress with
// ZSTD_decompressMultiFrame(dctx, dst, dstCapacity, src, srcSize, dict, dictSize, ddict) at
// main+0xEB380. Each of its three callers re-initialises a static DCtx (default parameters, format
// zstd1) right before the call and only uses the return value, so a default host DCtx without a
// dictionary decodes the same bytes. svc 0xF0 + i replaces the bl at kCallSites[i].
constexpr u64 kOotProgramId = 0x0400486024DEC000ULL;
constexpr u64 kMultiFrameOffset = 0xEB380;
constexpr std::array<u64, 3> kCallSites{0xBF72E0, 0xF50164, 0xE910C};
constexpr u32 kFirstSvc = 0xF0;
constexpr size_t kMaxSize = size_t{512} << 20;

struct DCtxDeleter {
    void operator()(ZSTD_DCtx* dctx) const {
        ZSTD_freeDCtx(dctx);
    }
};

// Only zstd1 and skippable frames, each complete: anything else (legacy formats the host library
// would accept, truncated input, trailing bytes) goes to the game's decoder.
bool OnlyStandardFrames(const u8* src, size_t size) {
    if (size == 0) {
        return false;
    }
    while (size != 0) {
        if (size < 4) {
            return false;
        }
        u32 magic;
        std::memcpy(&magic, src, sizeof(magic));
        if (magic != ZSTD_MAGICNUMBER &&
            (magic & ZSTD_MAGIC_SKIPPABLE_MASK) != ZSTD_MAGIC_SKIPPABLE_START) {
            return false;
        }
        const size_t frame_size = ZSTD_findFrameCompressedSize(src, size);
        if (ZSTD_isError(frame_size) || frame_size == 0 || frame_size > size) {
            return false;
        }
        src += frame_size;
        size -= frame_size;
    }
    return true;
}

// Decompresses on the host. False when the game's own function must run instead; then nothing was
// written to guest memory. On success args[0] holds the decompressed size, as the function returns.
bool HostDecompress(Core::Memory::Memory& memory, std::span<uint64_t, 8> args) {
    const u64 dst = args[1];
    const u64 capacity = args[2];
    const u64 src = args[3];
    const u64 src_size = args[4];
    if (args[5] != 0 || args[6] != 0 || args[7] != 0 || src_size == 0 || src_size > kMaxSize ||
        !memory.IsValidVirtualAddressRange(src, src_size)) {
        return false;
    }
    thread_local Common::ScratchBuffer<u8> input;
    thread_local Common::ScratchBuffer<u8> output;
    thread_local std::unique_ptr<ZSTD_DCtx, DCtxDeleter> dctx{ZSTD_createDCtx()};
    if (!dctx) {
        return false;
    }
    input.resize_destructive(src_size);
    memory.ReadBlock(src, input.data(), src_size);
    if (!OnlyStandardFrames(input.data(), src_size)) {
        return false;
    }
    // Same result for any capacity >= the output, and a capacity below it fails either way.
    const unsigned long long bound = ZSTD_decompressBound(input.data(), src_size);
    if (bound == ZSTD_CONTENTSIZE_ERROR) {
        return false;
    }
    const u64 out_capacity = std::min<u64>(capacity, bound);
    if (out_capacity > kMaxSize) {
        return false;
    }
    output.resize_destructive(std::max<size_t>(out_capacity, 1));
    const size_t result = ZSTD_decompressDCtx(dctx.get(), output.data(), out_capacity,
                                              input.data(), src_size);
    if (ZSTD_isError(result)) {
        return false;
    }
    if (result != 0) {
        if (!memory.IsValidVirtualAddressRange(dst, result)) {
            return false;
        }
        memory.WriteBlock(dst, output.data(), result);
    }
    args[0] = result;
    return true;
}

// Runs the game's function as the replaced bl would have: lr = the instruction after the svc.
void RunGuestFunction(Core::System& system, KProcess& process, u32 imm) {
    auto* const arm = process.GetArmInterface(system.Kernel().CurrentPhysicalCoreIndex());
    ThreadContext ctx{};
    arm->GetContext(ctx);
    const u64 return_address = ctx.pc;
    ctx.lr = return_address;
    ctx.pc = return_address - 4 - kCallSites[imm - kFirstSvc] + kMultiFrameOffset;
    arm->SetContext(ctx);
}

} // Anonymous namespace

bool CallNx2OotZstd(Core::System& system, u32 imm, std::span<uint64_t, 8> args) {
    if (imm < kFirstSvc || imm >= kFirstSvc + kCallSites.size()) {
        return false;
    }
    auto& process = GetCurrentProcess(system.Kernel());
    if (!process.Is64Bit() || process.GetProgramId() != kOotProgramId) {
        return false;
    }
    static std::atomic<u64> calls{};
    static std::atomic<u64> fallbacks{};
    static std::atomic<u64> bytes{};
    const u64 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (HostDecompress(process.GetMemory(), args)) {
        bytes.fetch_add(args[0], std::memory_order_relaxed);
    } else {
        if (fallbacks.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_WARNING(Kernel_SVC,
                        "Native zstd (svc {:#x}): src={:#x} size={:#x} dst={:#x} cap={:#x} "
                        "dict={:#x}/{:#x} ddict={:#x}; running the game's decoder",
                        imm, args[3], args[4], args[1], args[2], args[5], args[6], args[7]);
        }
        RunGuestFunction(system, process, imm);
    }
    if (n == 1 || n % 20000 == 0) {
        LOG_INFO(Kernel_SVC, "Native zstd: {} calls, {} MiB decompressed, {} by the game", n,
                 bytes.load(std::memory_order_relaxed) >> 20,
                 fallbacks.load(std::memory_order_relaxed));
    }
    return true;
}

} // namespace Kernel::Svc
