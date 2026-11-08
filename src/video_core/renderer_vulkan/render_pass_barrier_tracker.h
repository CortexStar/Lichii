// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include <vulkan/vulkan_core.h>

#include "common/common_types.h"

namespace Vulkan {

/**
 * Decides when the barrier after a render pass (its attachment writes, and everything recorded
 * before it, ordered before later graphics and compute work) has to be recorded.
 *
 * Render passes have no external subpass dependency, so this barrier is all that orders one pass
 * after another. It is needed before the next work that:
 * - uses a pending attachment as an attachment again (write after write, load after write),
 * - writes as an attachment an image that a draw since the barrier read (write after read),
 * - samples or stores to a pending attachment (read after write),
 * - writes storage memory or transform feedback buffers, whose earlier readers are unknown,
 * - is anything else than a tracked draw (dispatches, copies, untracked render passes).
 * Until then, passes on other attachments start without it. All pending passes share one barrier.
 */
class RenderPassBarrierTracker {
public:
    /// True while a render pass's barrier is deferred.
    [[nodiscard]] bool Pending() const {
        return pending;
    }

    /**
     * A render pass ended.
     * @param attachments Its attachments.
     * @param reads_known Every draw of the pass reported the images it uses (NoteDraw).
     * @return True when the barrier has to be recorded now.
     */
    bool EndPass(std::span<const VkImage> attachments, bool reads_known) {
        pending = true;
        ++epoch;
        for (const VkImage image : attachments) {
            writes.Insert(image);
        }
        return !reads_known || writes.Full() || reads.Full();
    }

    /// Whether a render pass on attachments needs the deferred barrier before it begins.
    [[nodiscard]] bool BeginNeedsBarrier(std::span<const VkImage> attachments) const {
        if (!pending) {
            return false;
        }
        for (const VkImage image : attachments) {
            if (writes.Contains(image) || reads.Contains(image)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Whether a draw needs the deferred barrier before it.
     * @param images       Images the draw samples or uses as storage images.
     * @param other_writes The draw writes storage buffers, storage images or transform feedback.
     */
    [[nodiscard]] bool DrawNeedsBarrier(std::span<const VkImage> images, bool other_writes) const {
        if (!pending) {
            return false;
        }
        if (other_writes) {
            return true;
        }
        for (const VkImage image : images) {
            if (writes.Contains(image)) {
                return true;
            }
        }
        return false;
    }

    /// Notes the images a draw samples or uses as storage images.
    void NoteDraw(std::span<const VkImage> images) {
        for (const VkImage image : images) {
            reads.Insert(image);
        }
    }

    /**
     * Whether DrawNeedsBarrier and NoteDraw can be skipped for a draw with `images`: the last draw
     * checked them, and no pass ended and no barrier was recorded since. DrawNeedsBarrier then
     * gives the same answer (no barrier, or one would have cleared the sets) and NoteDraw has
     * nothing to add. Draws mostly sample what the draw before them sampled.
     */
    [[nodiscard]] bool SameAsCheckedDraw(std::span<const VkImage> images) const {
        return checked_epoch == epoch && images.size() == checked_count &&
               std::equal(images.begin(), images.end(), checked_images.begin());
    }

    /// The draw with `images` was checked and noted; NoteCheckedDraw's images if they all fit.
    void NoteCheckedDraw(std::span<const VkImage> images) {
        if (images.size() > checked_images.size()) {
            checked_epoch = ~u64{0};
            return;
        }
        std::copy(images.begin(), images.end(), checked_images.begin());
        checked_count = images.size();
        checked_epoch = epoch;
    }

    /// The barrier was recorded: everything before it is ordered.
    void Clear() {
        pending = false;
        writes.Clear();
        reads.Clear();
        ++epoch;
    }

private:
    /// Open-addressing set of image handles. Inserts past three quarters of the slots are dropped
    /// and mark the set full; a full set always asks for the barrier.
    class ImageSet {
    public:
        void Insert(VkImage image) {
            if (image == VK_NULL_HANDLE) {
                return;
            }
            if (count >= MaxCount) {
                full = true;
                return;
            }
            for (size_t slot = Hash(image);; slot = (slot + 1) & (Slots - 1)) {
                if (slots[slot] == image) {
                    return;
                }
                if (slots[slot] == VK_NULL_HANDLE) {
                    slots[slot] = image;
                    ++count;
                    return;
                }
            }
        }

        [[nodiscard]] bool Contains(VkImage image) const {
            if (full) {
                return true;
            }
            if (image == VK_NULL_HANDLE || count == 0) {
                return false;
            }
            for (size_t slot = Hash(image);; slot = (slot + 1) & (Slots - 1)) {
                if (slots[slot] == image) {
                    return true;
                }
                if (slots[slot] == VK_NULL_HANDLE) {
                    return false;
                }
            }
        }

        [[nodiscard]] bool Full() const {
            return full;
        }

        void Clear() {
            if (count != 0) {
                slots.fill(VK_NULL_HANDLE);
            }
            count = 0;
            full = false;
        }

    private:
        static constexpr size_t Slots = 256;
        static constexpr size_t MaxCount = Slots * 3 / 4;

        static size_t Hash(VkImage image) {
            // A pointer or a 64-bit integer, depending on the platform.
            u64 key{};
            std::memcpy(&key, &image, sizeof(image));
            return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 56) & (Slots - 1);
        }

        std::array<VkImage, Slots> slots{};
        size_t count{};
        bool full{};
    };

    ImageSet writes;
    ImageSet reads;
    bool pending{};
    /// Moves whenever the sets or `pending` can change (EndPass, Clear).
    u64 epoch{};
    std::array<VkImage, 32> checked_images{};
    size_t checked_count{};
    u64 checked_epoch{~u64{0}};
};

} // namespace Vulkan
