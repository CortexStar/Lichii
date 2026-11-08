// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "video_core/native_qmd.h"

namespace VideoCommon {

/// The QMD scheduling of every channel (Tegra::NativeQmdScheduler), run on the split GPU thread's
/// front as its back will run it, on a table of its own: what the launches release is known when
/// they are sent, and goes to the semaphore prediction (GpuSplit::NoteRelease, PredictedRelease)
/// and the narrow drains' write index (GpuSplit::NoteWrite) like any other release.
///
/// The table is the back's: an engine's front takes it over with the back drained when it takes
/// the engine's macros over (TakeOver), and again whenever the back has run everything sent while
/// the view lost it (MaybeTakeOver). It stays the back's as long as the front reads the
/// descriptors the back will read. The front reads one when it sends a signal, the back when it
/// runs it, both from guest memory without the GPU caches. The front takes a descriptor:
/// - from the signal's own inline QMD, which the back writes right before;
/// - from memory, once the back ran every write sent there (GpuSplit::QmdWritesTarget: the
///   recorded writes over it, and every write that may land in guest memory unbounded or change
///   the mappings), on pages GPU work never wrote (Nx2MayBeGpuWritten);
/// - from the inline QMD a LOAD_INLINE_QMD_DATA still queued writes there, when that is the last
///   write sent there (NoteInlineWrite).
/// Where it can't, it loses the table: it stops valuing, and the launches hold semaphore
/// acquires as every dispatch did before (GpuSplit::NoteDispatch).
///
/// `Memory`, the channel's GPU address space as the back reads it, has:
/// - bool IsMapped(GPUVAddr, u64), void Read(GPUVAddr, void*, u64);
/// - std::optional<u64> Device(GPUVAddr), bool GpuWritten(u64 device, u64 size);
/// - u64 WritesTarget(const GpuSplit&, GPUVAddr, u64): GpuSplit::QmdWritesTarget;
/// - std::optional<u32> WordAt(GpuSplit&, GPUVAddr): GpuSplit::SemaphoreWordAt;
/// - void NoteWrite(GpuSplit&, GPUVAddr, u64 size), void NoteRelease(GpuSplit&, GPUVAddr, u32
///   value, bool at_least), void ForgetRelease(GpuSplit&, GPUVAddr).
class FrontQmdView {
public:
    using Descriptor = Tegra::NativeQmdScheduler::Descriptor;

    /// The view the compute engines of every channel feed: they share the back's table.
    static FrontQmdView& Get() {
        static FrontQmdView view;
        return view;
    }

    /// The back ran everything sent and runs nothing (the split drained, or it waits for work):
    /// takes over the table `back` shares with the other engines.
    void TakeOver(const Tegra::NativeQmdScheduler& back) {
        scheduler.CopyStates(back);
        synced = true;
    }
    /// The front, between commands and before QMD methods: takes the back's table over if the
    /// view lost it and the back has run everything sent.
    void MaybeTakeOver(GpuSplit& split) {
        if (!synced && split.Ran(split.Position())) [[unlikely]] {
            TakeOver(back_table);
        }
    }

    /// The engines schedule QMDs the front does not see (the split stopped).
    void Lose() {
        synced = false;
        inline_writes_count = 0;
    }

    [[nodiscard]] bool Synced() const {
        return synced;
    }

    /// LOAD_INLINE_QMD_DATA was just sent: running it, the back writes `qmd` to the QMD at
    /// `device`. Until then the front reads it from here (ReadDescriptor).
    void NoteInlineWrite(const GpuSplit& split, u64 device, const Descriptor& qmd) {
        InlineWrite& written = inline_writes[inline_writes_count % inline_writes.size()];
        ++inline_writes_count;
        written.device = device;
        written.position = split.Position();
        written.qmd = qmd;
        written.overwritten = false;
    }

    /// The scheduling of PCAS action `argument` on the QMD at `address` that the back runs at the
    /// entry just sent (KeplerCompute::ProcessNativeMethod); `inline_qmd`: the QMD it writes there
    /// right before. Notes what the launches it runs release. Returns whether it did.
    template <typename Memory>
    bool Signal(GpuSplit& split, const Memory& memory, GPUVAddr address, u32 argument,
                const Descriptor* inline_qmd) {
        if (!synced) {
            return false;
        }
        // The back must find the mappings the front reads with.
        if (!split.Ran(split.MappingPosition())) {
            Lose();
            return false;
        }
        const u64 position = split.Position();
        releases.clear();
        bool exact = true;
        const auto read = [&](u64 qmd_address, Descriptor& descriptor) {
            if (!memory.IsMapped(qmd_address, sizeof(descriptor))) {
                return false;
            }
            if (inline_qmd != nullptr && qmd_address == address) {
                descriptor = *inline_qmd;
            } else if (!ReadDescriptor(split, memory, qmd_address, position, descriptor)) {
                exact = false;
                return false; // Stops the scheduling: the table is lost
            }
            // A release this signal ran before writes into the QMD.
            for (const Release& earlier : releases) {
                if (earlier.address + 16 > qmd_address &&
                    earlier.address < qmd_address + sizeof(descriptor)) {
                    exact = false;
                    return false;
                }
            }
            return true;
        };
        const auto release = [&](u64 destination, u32 payload, u32 flags) {
            const bool four_words = (flags >> 30) == 0;
            if (!memory.IsMapped(destination, four_words ? 16 : sizeof(u32))) {
                return false;
            }
            releases.push_back({destination, payload, flags});
            return true;
        };
        scheduler.Signal(address, argument, read, release, [](const Descriptor&) { return true; });
        if (!exact) {
            Lose();
            return false;
        }
        for (const Release& noted : releases) {
            NoteRelease(split, memory, noted.address, noted.payload, noted.flags);
        }
        return true;
    }

private:
    struct Release {
        GPUVAddr address;
        u32 payload;
        u32 flags;
    };

    /// An inline QMD a LOAD_INLINE_QMD_DATA sent writes (NoteInlineWrite).
    struct InlineWrite {
        u64 device{};
        u64 position{};
        Descriptor qmd{};
        bool overwritten{}; ///< A release of the same entry writes into it after
    };

    /// Reads into `descriptor` the QMD at `qmd_address` as the back will read it running the
    /// signal sent at `position`, or tells it can't.
    template <typename Memory>
    [[nodiscard]] bool ReadDescriptor(GpuSplit& split, const Memory& memory, GPUVAddr qmd_address,
                                      u64 position, Descriptor& descriptor) const {
        const auto device = memory.Device(qmd_address);
        if (!device || memory.GpuWritten(*device, sizeof(descriptor))) {
            return false;
        }
        // The last write sent there, or that may land anywhere.
        const u64 target = memory.WritesTarget(split, qmd_address, sizeof(descriptor));
        if (target > position) {
            return false;
        }
        if (split.Ran(target)) {
            // Read only once Ran saw the back past the write: the back stores its position after
            // the writes before it, and an idle back may run a write published shortly before
            // the signal while the front reads (#805's 611a09d4a0).
            memory.Read(qmd_address, descriptor.data(), sizeof(descriptor));
            return true;
        }
        if (const InlineWrite* written = QueuedInlineWrite(split, *device);
            written != nullptr && written->position == target) {
            descriptor = written->qmd;
            return true;
        }
        return false;
    }

    /// The last inline QMD sent to the QMD at `device` that the back has not written yet.
    [[nodiscard]] const InlineWrite* QueuedInlineWrite(GpuSplit& split, u64 device) const {
        const size_t count = (std::min)(inline_writes_count, inline_writes.size());
        for (size_t i = 1; i <= count; ++i) {
            const InlineWrite& written =
                inline_writes[(inline_writes_count - i) % inline_writes.size()];
            if (split.Ran(written.position)) {
                break; // The back wrote it, and the older ones
            }
            if (written.device == device) {
                return written.overwritten ? nullptr : &written;
            }
        }
        return nullptr;
    }

    /// A release the back runs with the entry just sent, as ProcessNativeMethod's: what the word
    /// holds once the back ran it and its fence signalled.
    template <typename Memory>
    void NoteRelease(GpuSplit& split, const Memory& memory, GPUVAddr address, u32 payload,
                     u32 flags) {
        const bool four_words = (flags >> 30) == 0;
        const bool reduction = (flags & (1U << 26)) != 0;
        const bool maximum = !reduction && !four_words && (flags & (1U << 19)) != 0;
        const u64 size = four_words ? 16 : sizeof(u32);
        // The word a reduction or a maximum combines the payload with: the back reads it when
        // the release's fence signals, after every write sent before landed.
        std::optional<u32> base;
        if (reduction || maximum) {
            base = memory.WordAt(split, address);
        }
        memory.NoteWrite(split, address, size);
        if (const auto device = memory.Device(address)) {
            // An inline QMD this entry wrote before is no longer what memory holds there.
            const size_t count = (std::min)(inline_writes_count, inline_writes.size());
            for (size_t i = 1; i <= count; ++i) {
                InlineWrite& written =
                    inline_writes[(inline_writes_count - i) % inline_writes.size()];
                if (written.position != split.Position()) {
                    break;
                }
                if (written.device < *device + size &&
                    *device < written.device + sizeof(Descriptor)) {
                    written.overwritten = true;
                }
            }
        }
        if (reduction) {
            if (base) {
                const bool is_signed = ((flags >> 24) & 1) != 0;
                memory.NoteRelease(
                    split, address,
                    Tegra::ReduceSemaphore((flags >> 20) & 7, is_signed, *base, payload), false);
            } else {
                memory.ForgetRelease(split, address);
            }
        } else if (maximum) {
            // Whatever the word held, it holds the payload at least.
            memory.NoteRelease(split, address, base ? (std::max)(*base, payload) : payload,
                               !base.has_value());
        } else {
            // Four words: the payload as 64 bits, a time stamp after it.
            memory.NoteRelease(split, address, payload, false);
        }
    }

    Tegra::NativeQmdScheduler scheduler{Tegra::NativeQmdScheduler::OwnTable{}};
    /// The table the engines of every channel share, which the back's scheduling uses.
    Tegra::NativeQmdScheduler back_table;
    std::vector<Release> releases;
    bool synced{};
    /// The last inline QMDs sent, oldest first from inline_writes_count on: a back a millisecond
    /// behind has some ten to write, a few times that in long frames.
    std::array<InlineWrite, 256> inline_writes{};
    size_t inline_writes_count{};
};

} // namespace VideoCommon
