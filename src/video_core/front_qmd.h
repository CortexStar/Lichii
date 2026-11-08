// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "video_core/native_qmd.h"
#include "video_core/nx2_profile.h"

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
/// No change of the mappings may be queued either (GpuSplit::MappingPosition). Memory holds what
/// the back finds there only while the back has not run the signal's entry yet: the front sends
/// and values it under a GpuSplit::PublishHold.
///
/// Where the back has yet to run a write the front can't read past (into a QMD the signal reads,
/// unbounded, or to the mappings), the signal waits, with what it changed in the table taken
/// back (NativeQmdScheduler::BeginAttempt), and every signal sent after it waits behind it: the
/// table stays the back's as of the signals valued. Waiting launches hold semaphore acquires as
/// every dispatch did before (GpuSplit::NoteDispatch). The front values the waiting ones in order
/// once the back ran what the first waits for (CatchUp). Late, the back may have run the signal
/// and what followed it, so memory holds what the back read only where nothing sent after the
/// signal writes the QMD, and no release of the waiting signals writes a QMD read late. Their
/// releases are placed at their signals' positions (GpuSplit::NoteWriteAt), not predicted. Where
/// the front can't read what the back read at all (a QMD on a GPU-written page, written after
/// the signal, released into, or the mappings changed after it), it loses the table: it stops
/// valuing, and takes the back's table over the next time the back has run everything sent.
///
/// `Memory`, the channel's GPU address space as the back reads it, has:
/// - bool IsMapped(GPUVAddr, u64), void Read(GPUVAddr, void*, u64);
/// - std::optional<u64> Device(GPUVAddr), bool GpuWritten(u64 device, u64 size);
/// - u64 WritesTarget(const GpuSplit&, GPUVAddr, u64): GpuSplit::QmdWritesTarget;
/// - std::optional<u32> WordAt(GpuSplit&, GPUVAddr): GpuSplit::SemaphoreWordAt;
/// - void NoteWrite(GpuSplit&, GPUVAddr, u64 size), void NoteWriteAt(GpuSplit&, GPUVAddr, u64
///   size, u64 position), void NoteRelease(GpuSplit&, GPUVAddr, u32 value, bool at_least),
///   void ForgetRelease(GpuSplit&, GPUVAddr);
/// - const void* Id(): the address space's, the same for every Memory of it.
class FrontQmdView {
public:
    using Descriptor = Tegra::NativeQmdScheduler::Descriptor;

    /// The view the compute engines of every channel feed: they share the back's table.
    static FrontQmdView& Get() {
        static FrontQmdView view;
        return view;
    }

    /// The back ran everything sent and runs nothing (the split drained, or it waits for work):
    /// takes over the table `back` shares with the other engines. What waits, the back ran.
    void TakeOver(const Tegra::NativeQmdScheduler& back) {
        // EXPERIMENT (do not merge): EDEN_NX2_QMD_VIEW=0 never takes a table over, so every QMD
        // signal holds semaphore acquires as before the view.
        static const bool view_off = [] {
            const char* const value = std::getenv("EDEN_NX2_QMD_VIEW");
            return value != nullptr && value[0] == '0';
        }();
        if (view_off) {
            return;
        }
        scheduler.CopyStates(back);
        synced = true;
        ClearWaiting();
    }
    /// The front, between commands and before QMD methods: takes the back's table over if the
    /// view lost it or signals wait, and the back has run everything sent.
    void MaybeTakeOver(GpuSplit& split) {
        if (!Current() && split.Ran(split.Position())) [[unlikely]] {
            TakeOver(back_table);
        }
    }

    /// The engines schedule QMDs the front does not see (the split stopped).
    void Lose() {
        if (synced) {
            Nx2Profile::Add(Nx2Profile::Counter::QmdLost, 1);
        }
        synced = false;
        inline_writes_count = 0;
        ClearWaiting();
    }

    [[nodiscard]] bool Synced() const {
        return synced;
    }
    /// Whether the table is the back's as of everything sent: synced, and no signal waits.
    [[nodiscard]] bool Current() const {
        return synced && waiting.empty();
    }
    [[nodiscard]] size_t Waiting() const {
        return waiting.size();
    }

    /// EDEN_NX2_DRAIN_CHECK (diagnostic): what the last signal Signal valued read and noted, for
    /// the back to compare with what it reads and releases running it.
    struct CheckRead {
        GPUVAddr address;
        Descriptor descriptor;
    };
    struct CheckNote {
        GPUVAddr address;
        u32 payload; ///< The release's own
        u32 flags;
        u32 value; ///< What the word holds once it landed, if `known`
        bool known;
        bool at_least;
    };
    struct Check {
        std::vector<CheckRead> reads;
        std::vector<CheckNote> notes;
    };
    void EnableChecks() {
        checking = true;
    }
    [[nodiscard]] Check TakeCheck() {
        return std::exchange(check, {});
    }
    /// A copy of the table, while it is current (as the back's once it ran everything sent).
    [[nodiscard]] std::shared_ptr<Tegra::NativeQmdScheduler> CopyTable() const {
        auto copy =
            std::make_shared<Tegra::NativeQmdScheduler>(Tegra::NativeQmdScheduler::OwnTable{});
        copy->CopyStates(scheduler);
        return copy;
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
    /// right before. Notes what the launches it runs release, or has the signal wait. Returns
    /// whether it noted them. The entry must still be unpublished (GpuSplit::PublishHold).
    template <typename Memory>
    bool Signal(GpuSplit& split, const Memory& memory, GPUVAddr address, u32 argument,
                const Descriptor* inline_qmd) {
        if (synced) {
            CatchUp(split, memory);
        }
        if (!synced) {
            return false;
        }
        const u64 position = split.Position();
        if (!waiting.empty()) {
            // Behind a signal still waiting.
            Wait(position, memory.Id(), address, argument, inline_qmd);
            return false;
        }
        const Outcome outcome =
            Attempt(split, memory, position, address, argument, inline_qmd, false);
        if (outcome.valued) {
            return true;
        }
        if (outcome.retry_after) {
            retry_after = *outcome.retry_after;
            Wait(position, memory.Id(), address, argument, inline_qmd);
            return false;
        }
        Lose();
        return false;
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

    /// A signal sent at `position` that waits to be valued (`inline_qmd` if `has_inline`).
    struct WaitingSignal {
        u64 position;
        const void* memory; ///< Memory::Id of the address space it reads
        GPUVAddr address;
        u32 argument;
        bool has_inline;
        Descriptor inline_qmd;
    };

    /// How an attempt to value a signal ended: valued, or taken back, to try again once the back
    /// ran `retry_after`, or for good (the view loses its table).
    struct Outcome {
        bool valued;
        std::optional<u64> retry_after;
    };

    /// Signals that wait, at most: a back that far behind leaves the view to its next takeover.
    static constexpr size_t MaxWaiting = 4096;

    /// The signal sent at `position` waits. None of its releases is noted: they are placed once
    /// it is valued.
    void Wait(u64 position, const void* memory_id, GPUVAddr address, u32 argument,
              const Descriptor* inline_qmd) {
        if (waiting.size() >= MaxWaiting) {
            Lose();
            return;
        }
        WaitingSignal& signal = waiting.emplace_back();
        signal.position = position;
        signal.memory = memory_id;
        signal.address = address;
        signal.argument = argument;
        signal.has_inline = inline_qmd != nullptr;
        if (inline_qmd != nullptr) {
            signal.inline_qmd = *inline_qmd;
        }
    }

    void ClearWaiting() {
        waiting.clear();
        late_reads.clear();
        retry_after = 0;
    }

    /// Values the waiting signals that read `memory` in order, from the first, as long as the back
    /// ran what the first waits for.
    template <typename Memory>
    void CatchUp(GpuSplit& split, const Memory& memory) {
        while (!waiting.empty()) {
            const WaitingSignal& first = waiting.front();
            if (first.memory != memory.Id() || !split.Ran(retry_after)) {
                return;
            }
            const Outcome outcome =
                Attempt(split, memory, first.position, first.address, first.argument,
                        first.has_inline ? &first.inline_qmd : nullptr, true);
            if (outcome.retry_after) {
                retry_after = *outcome.retry_after;
                return;
            }
            if (!outcome.valued) {
                Lose();
                return;
            }
            waiting.pop_front();
            retry_after = 0;
            Nx2Profile::Add(Nx2Profile::Counter::QmdLate, 1);
        }
        // Every signal sent before the reads made late is valued: none of their releases waits.
        late_reads.clear();
    }

    /// Runs the signal sent at `position` on the table as the back runs it, and notes what its
    /// launches release, or places it if `late`. Takes it back where the front can't read a QMD
    /// as the back reads it.
    template <typename Memory>
    Outcome Attempt(GpuSplit& split, const Memory& memory, u64 position, GPUVAddr address,
                    u32 argument, const Descriptor* inline_qmd, bool late) {
        // The back finds the mappings every change sent before the signal made, and none after it.
        const u64 mapping_change = split.MappingPosition();
        if (mapping_change > position) {
            return {false, std::nullopt};
        }
        if (!split.Ran(mapping_change)) {
            return {false, mapping_change};
        }
        releases.clear();
        attempt_reads.clear();
        std::optional<Outcome> failure;
        const auto read = [&](u64 qmd_address, Descriptor& descriptor) {
            if (!memory.IsMapped(qmd_address, sizeof(descriptor))) {
                return false;
            }
            if (inline_qmd != nullptr && qmd_address == address) {
                descriptor = *inline_qmd;
            } else if (const auto unread =
                           ReadDescriptor(split, memory, qmd_address, position, descriptor)) {
                failure = unread;
                return false; // Stops the scheduling, which is taken back
            }
            // A release this signal ran before writes into the QMD.
            for (const Release& earlier : releases) {
                if (earlier.address + 16 > qmd_address &&
                    earlier.address < qmd_address + sizeof(descriptor)) {
                    failure = Outcome{false, std::nullopt};
                    return false;
                }
            }
            attempt_reads.push_back(qmd_address);
            if (checking && !late) {
                check.reads.push_back({qmd_address, descriptor});
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
        if (checking) {
            check = {};
        }
        scheduler.BeginAttempt();
        scheduler.Signal(address, argument, read, release, [](const Descriptor&) { return true; });
        if (!failure && late && ReleasesIntoLateReads()) {
            failure = Outcome{false, std::nullopt};
        }
        if (failure) {
            scheduler.RollbackAttempt();
            return *failure;
        }
        scheduler.CommitAttempt();
        if (late) {
            for (const u64 read_address : attempt_reads) {
                late_reads.insert(read_address >> 8);
            }
            for (const Release& done : releases) {
                memory.NoteWriteAt(split, done.address, (done.flags >> 30) == 0 ? 16 : 4, position);
            }
        } else {
            for (const Release& noted : releases) {
                NoteRelease(split, memory, noted.address, noted.payload, noted.flags);
            }
        }
        return {true, std::nullopt};
    }

    /// Late: whether a release of the signal writes a QMD it read, or one a signal valued late
    /// before read. The back may have run the release before the front read that QMD.
    [[nodiscard]] bool ReleasesIntoLateReads() const {
        for (const Release& done : releases) {
            const u64 first = done.address >> 8;
            const u64 last = (done.address + 15) >> 8;
            for (const u64 read_address : attempt_reads) {
                if ((read_address >> 8) == first || (read_address >> 8) == last) {
                    return true;
                }
            }
            if (late_reads.contains(first) || late_reads.contains(last)) {
                return true;
            }
        }
        return false;
    }

    /// Reads into `descriptor` the QMD at `qmd_address` as the back read it, or will, running the
    /// signal sent at `position`, or tells why the front can't.
    template <typename Memory>
    [[nodiscard]] std::optional<Outcome> ReadDescriptor(GpuSplit& split, const Memory& memory,
                                                        GPUVAddr qmd_address, u64 position,
                                                        Descriptor& descriptor) const {
        const auto device = memory.Device(qmd_address);
        if (!device || memory.GpuWritten(*device, sizeof(descriptor))) {
            return Outcome{false, std::nullopt};
        }
        // The last write sent there, or that may land anywhere.
        const u64 target = memory.WritesTarget(split, qmd_address, sizeof(descriptor));
        if (target > position) {
            // Sent after the signal (it waited): memory may hold it, the back read what was before.
            return Outcome{false, std::nullopt};
        }
        if (split.Ran(target)) {
            // Read only once Ran saw the back past the write: the back stores its position after
            // the writes before it, and an idle back may run a write published shortly before
            // the signal while the front reads (#805's 611a09d4a0).
            memory.Read(qmd_address, descriptor.data(), sizeof(descriptor));
            return std::nullopt;
        }
        if (const InlineWrite* written = QueuedInlineWrite(split, *device);
            written != nullptr && written->position == target) {
            descriptor = written->qmd;
            return std::nullopt;
        }
        return Outcome{false, target};
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
        CheckNote note{address, payload, flags, payload, true, false};
        if (reduction) {
            if (base) {
                const bool is_signed = ((flags >> 24) & 1) != 0;
                note.value = Tegra::ReduceSemaphore((flags >> 20) & 7, is_signed, *base, payload);
                memory.NoteRelease(split, address, note.value, false);
            } else {
                note.known = false;
                memory.ForgetRelease(split, address);
            }
        } else if (maximum) {
            // Whatever the word held, it holds the payload at least.
            note.value = base ? (std::max)(*base, payload) : payload;
            note.at_least = !base.has_value();
            memory.NoteRelease(split, address, note.value, note.at_least);
        } else {
            // Four words: the payload as 64 bits, a time stamp after it.
            memory.NoteRelease(split, address, payload, false);
        }
        if (checking) {
            check.notes.push_back(note);
        }
    }

    Tegra::NativeQmdScheduler scheduler{Tegra::NativeQmdScheduler::OwnTable{}};
    /// The table the engines of every channel share, which the back's scheduling uses.
    Tegra::NativeQmdScheduler back_table;
    std::vector<Release> releases;
    std::vector<u64> attempt_reads; ///< The QMDs the attempt read
    bool synced{};
    /// Signals that wait to be valued, oldest first; the first may be once the back ran
    /// retry_after. late_reads: the granules of the QMDs read late since the waiting began.
    std::deque<WaitingSignal> waiting;
    u64 retry_after{};
    std::unordered_set<u64> late_reads;
    bool checking{};
    Check check;
    /// The last inline QMDs sent, oldest first from inline_writes_count on: a back a millisecond
    /// behind has some ten to write, a few times that in long frames.
    std::array<InlineWrite, 256> inline_writes{};
    size_t inline_writes_count{};
};

} // namespace VideoCommon
