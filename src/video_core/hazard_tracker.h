// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace VideoCommon {

/**
 * Decides whether GPU work (a guest dispatch; later pass ends) needs the barrier that orders
 * earlier work before it, from what it accesses against what the work since the last barrier
 * accessed.
 *
 * The barrier is needed before the work when:
 * - anything was recorded since the last barrier whose accesses are not tracked here (the caller
 *   passes other_work),
 * - it reads or writes a range or image that tracked work since the barrier wrote (read after
 *   write, write after write),
 * - it writes a range or image that tracked work since the barrier read (write after read),
 * - its accesses are not fully known, or more accesses are pending than the tracker holds.
 * Otherwise it runs without one, overlapping the work before it. Ranges are in one address space
 * in which equal addresses are the same memory (device addresses of the buffer cache); images are
 * opaque handles of distinct resources.
 */
class HazardTracker {
public:
    struct Range {
        u64 begin;
        u64 end; ///< Exclusive
    };

    /// The accesses of one piece of work, gathered before the decision.
    class Accesses {
    public:
        void ReadBuffer(u64 begin, u64 size) {
            Add(buffer_reads, num_buffer_reads, begin, size);
        }
        void WriteBuffer(u64 begin, u64 size) {
            Add(buffer_writes, num_buffer_writes, begin, size);
        }
        void ReadImage(u64 image) {
            Add(image_reads, num_image_reads, image);
        }
        void WriteImage(u64 image) {
            Add(image_writes, num_image_writes, image);
        }
        /// The work also accesses memory that is not listed.
        void MarkUnknown() {
            unknown = true;
        }
        void Clear() {
            num_buffer_reads = num_buffer_writes = num_image_reads = num_image_writes = 0;
            unknown = false;
        }

    private:
        friend class HazardTracker;
        static constexpr size_t Capacity = 64;

        void Add(std::array<Range, Capacity>& list, size_t& count, u64 begin, u64 size) {
            if (size == 0) {
                return;
            }
            if (count == Capacity) {
                unknown = true;
                return;
            }
            list[count++] = {begin, begin + size};
        }
        void Add(std::array<u64, Capacity>& list, size_t& count, u64 image) {
            if (image == 0) {
                return;
            }
            if (count == Capacity) {
                unknown = true;
                return;
            }
            list[count++] = image;
        }

        std::array<Range, Capacity> buffer_reads{};
        std::array<Range, Capacity> buffer_writes{};
        std::array<u64, Capacity> image_reads{};
        std::array<u64, Capacity> image_writes{};
        size_t num_buffer_reads{};
        size_t num_buffer_writes{};
        size_t num_image_reads{};
        size_t num_image_writes{};
        bool unknown{};
    };

    enum class Decision : u32 {
        NoBarrier,       ///< Independent of all tracked work since the last barrier
        OtherWork,       ///< Untracked work was recorded since the last barrier, or none yet
        Unknown,         ///< The work's or the pending accesses are not fully known
        ReadAfterWrite,  ///< Reads what pending work wrote
        WriteAfterWrite, ///< Writes what pending work wrote
        WriteAfterRead,  ///< Writes what pending work read
        Count,
    };

    /**
     * Decides for the next work and notes its accesses as pending.
     * @param other_work Untracked work was recorded since the previous tracked work.
     * @return NoBarrier when the work can run without the barrier. Any other decision means the
     *         caller records the barrier; the work's accesses are then all that is pending. With
     *         other_work false, the barrier only has to order the earlier tracked work.
     */
    Decision Next(bool other_work, const Accesses& accesses) {
        Decision decision = Decide(other_work, accesses);
        if (decision != Decision::NoBarrier) {
            Clear();
        }
        Note(accesses);
        return decision;
    }

    /// A barrier ordering everything before it was recorded by someone else.
    void Clear() {
        num_buffer_reads = num_buffer_writes = num_image_reads = num_image_writes = 0;
        unknown = false;
        empty = true;
    }

private:
    static constexpr size_t Capacity = 256;

    static bool Overlaps(const Range& a, const Range& b) {
        return a.begin < b.end && b.begin < a.end;
    }

    template <size_t N, size_t M>
    static bool AnyOverlap(const std::array<Range, N>& a, size_t a_count,
                           const std::array<Range, M>& b, size_t b_count) {
        for (size_t i = 0; i < a_count; ++i) {
            for (size_t j = 0; j < b_count; ++j) {
                if (Overlaps(a[i], b[j])) {
                    return true;
                }
            }
        }
        return false;
    }

    template <size_t N, size_t M>
    static bool AnyEqual(const std::array<u64, N>& a, size_t a_count, const std::array<u64, M>& b,
                         size_t b_count) {
        for (size_t i = 0; i < a_count; ++i) {
            for (size_t j = 0; j < b_count; ++j) {
                if (a[i] == b[j]) {
                    return true;
                }
            }
        }
        return false;
    }

    Decision Decide(bool other_work, const Accesses& a) const {
        if (other_work || empty) {
            // Nothing pending is ordered yet, or unknown work ran: the barrier as it always was.
            return Decision::OtherWork;
        }
        if (a.unknown || unknown) {
            return Decision::Unknown;
        }
        if (AnyOverlap(a.buffer_reads, a.num_buffer_reads, buffer_writes, num_buffer_writes) ||
            AnyEqual(a.image_reads, a.num_image_reads, image_writes, num_image_writes)) {
            return Decision::ReadAfterWrite;
        }
        if (AnyOverlap(a.buffer_writes, a.num_buffer_writes, buffer_writes, num_buffer_writes) ||
            AnyEqual(a.image_writes, a.num_image_writes, image_writes, num_image_writes)) {
            return Decision::WriteAfterWrite;
        }
        if (AnyOverlap(a.buffer_writes, a.num_buffer_writes, buffer_reads, num_buffer_reads) ||
            AnyEqual(a.image_writes, a.num_image_writes, image_reads, num_image_reads)) {
            return Decision::WriteAfterRead;
        }
        return Decision::NoBarrier;
    }

    void Note(const Accesses& a) {
        empty = false;
        unknown |= a.unknown;
        Append(buffer_reads, num_buffer_reads, a.buffer_reads, a.num_buffer_reads);
        Append(buffer_writes, num_buffer_writes, a.buffer_writes, a.num_buffer_writes);
        Append(image_reads, num_image_reads, a.image_reads, a.num_image_reads);
        Append(image_writes, num_image_writes, a.image_writes, a.num_image_writes);
    }

    template <typename T, size_t N>
    void Append(std::array<T, Capacity>& list, size_t& count, const std::array<T, N>& from,
                size_t from_count) {
        for (size_t i = 0; i < from_count; ++i) {
            if (count == Capacity) {
                unknown = true;
                return;
            }
            list[count++] = from[i];
        }
    }

    std::array<Range, Capacity> buffer_reads{};
    std::array<Range, Capacity> buffer_writes{};
    std::array<u64, Capacity> image_reads{};
    std::array<u64, Capacity> image_writes{};
    size_t num_buffer_reads{};
    size_t num_buffer_writes{};
    size_t num_image_reads{};
    size_t num_image_writes{};
    bool unknown{};
    bool empty{true}; ///< No tracked work since the last barrier
};

} // namespace VideoCommon
