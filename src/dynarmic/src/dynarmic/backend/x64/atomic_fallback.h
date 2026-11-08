/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <type_traits>

#include "common/common_types.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/mcl/integer_of_size.hpp"

namespace Dynarmic::Backend::X64 {

/// What the LSE atomics' callback paths read: the callbacks, and the JIT's halt reasons (a refused
/// write stops retrying once the frontend halted for a memory abort).
struct AtomicFallbackContext {
    const A64::UserConfig* conf;
    const volatile u32* halt_reason;
};

template<std::size_t bitsize>
using AtomicT = mcl::unsigned_integer_of_size<bitsize>;

template<std::size_t bitsize>
bool AtomicWriteExclusive(A64::UserCallbacks* cb, u64 vaddr, AtomicT<bitsize> value, AtomicT<bitsize> expected) {
    if constexpr (bitsize == 8) {
        return cb->MemoryWriteExclusive8(vaddr, value, expected);
    } else if constexpr (bitsize == 16) {
        return cb->MemoryWriteExclusive16(vaddr, value, expected);
    } else if constexpr (bitsize == 32) {
        return cb->MemoryWriteExclusive32(vaddr, value, expected);
    } else {
        return cb->MemoryWriteExclusive64(vaddr, value, expected);
    }
}

// op is the LSE opc field: 0 ADD, 1 CLR, 2 EOR, 3 SET, 4 SMAX, 5 SMIN, 6 UMAX, 7 UMIN, 8 SWP.
template<std::size_t bitsize>
AtomicT<bitsize> AtomicRmwResult(AtomicT<bitsize> a, AtomicT<bitsize> b, u64 op) {
    using T = AtomicT<bitsize>;
    using S = std::make_signed_t<T>;
    switch (op) {
    case 0:
        return T(a + b);
    case 1:
        return T(a & ~b);
    case 2:
        return T(a ^ b);
    case 3:
        return T(a | b);
    case 4:
        return S(a) > S(b) ? a : b;
    case 5:
        return S(a) < S(b) ? a : b;
    case 6:
        return a > b ? a : b;
    case 7:
        return a < b ? a : b;
    default:
        return b;
    }
}

// Slow paths through the memory callbacks (no fastmem, or a fastmem fault): a CAS loop. The
// exclusive write is a compare-and-swap against the value read; when it fails, another core wrote
// the word in between, and the operation starts over from a new read. A refused write that left
// the word unchanged must not count as done: the word can change and change back between the
// write and a second read (a mutex locked and unlocked by another core while the callbacks
// handle a GPU-cached page), and the caller would then hold a lock it never took. Only a memory
// abort the frontend raised for the access (its abort check exits before the result is used)
// stops the retries.
inline bool AtomicAborted(const AtomicFallbackContext& context) {
    return (*context.halt_reason & static_cast<u32>(context.conf->memory_abort_halt_reasons)) != 0;
}

template<std::size_t bitsize>
u64 AtomicRmwFallback(const AtomicFallbackContext& context, u64 vaddr, u64 operand, u64 op) {
    using T = AtomicT<bitsize>;
    A64::UserCallbacks* const cb = context.conf->callbacks;
    for (;;) {
        const T old = T(cb->MemoryRead(vaddr, bitsize / 8));
        if (AtomicWriteExclusive<bitsize>(cb, vaddr, AtomicRmwResult<bitsize>(old, T(operand), op), old) ||
            AtomicAborted(context)) {
            return old;
        }
    }
}

template<std::size_t bitsize>
u64 AtomicCasFallback(const AtomicFallbackContext& context, u64 vaddr, u64 expected, u64 desired) {
    using T = AtomicT<bitsize>;
    A64::UserCallbacks* const cb = context.conf->callbacks;
    for (;;) {
        const T old = T(cb->MemoryRead(vaddr, bitsize / 8));
        if (old != T(expected)) {
            return old;
        }
        if (AtomicWriteExclusive<bitsize>(cb, vaddr, T(desired), old) || AtomicAborted(context)) {
            return old;
        }
    }
}

}  // namespace Dynarmic::Backend::X64
