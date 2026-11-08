// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace VideoCommon {

/// When a new image gets its contents from guest memory (JoinImages). Host memory of a new image
/// holds whatever its allocation held before (on AMD often another image's texels), so an image
/// must never be read or partly drawn before an upload or a write that covers all of it.
enum class InitialUpload {
    Now,        ///< Upload before the join copies run over it
    AtFirstUse, ///< Stays CpuModified: the first use uploads it, unless that use is a clear that
                ///< overwrites the whole image (PrepareImage with invalidate)
    Never,      ///< A join copy writes every texel
};

struct NewImageContents {
    bool copy_writes_whole;    ///< A join copy writes every texel of every level and layer
    bool binding_clear_covers; ///< Created while a clear binds it, and the clear covers it all
    bool has_join_copies;      ///< Alias or overlap copies run over the image after the upload
    bool stale_guest_memory;   ///< Every page is stale: a dropped GPU-written image's bytes
};

/// The upload rule for a new image. Stale guest memory is uploaded like any other unless the
/// first use overwrites the whole image: skipping it outright left texels undefined wherever
/// neither a join copy nor a draw wrote them (Ocarina's Load menu: 128x128 blocks of an earlier
/// picture over the blurred background).
[[nodiscard]] constexpr InitialUpload ChooseInitialUpload(const NewImageContents& image) {
    if (image.copy_writes_whole) {
        return InitialUpload::Never;
    }
    // Deferring past join copies would upload over what they copied.
    if (image.has_join_copies) {
        return InitialUpload::Now;
    }
    if (image.binding_clear_covers || image.stale_guest_memory) {
        return InitialUpload::AtFirstUse;
    }
    return InitialUpload::Now;
}

} // namespace VideoCommon
