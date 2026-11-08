// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"

namespace Service {

// The clock a video decoder session (nx2:vdecsvc) drops late frames by: the pts the movie's
// audio has reached. Times are host microseconds; pause_end is when emulation last resumed.
// Games such as Ocarina of Time take one decoded frame per frame they draw while the movie's audio
// plays in real time: below the movie's rate frames are dropped against this clock, above it
// frames are held back until it reaches them (HoldUs), before they are written: the decoder's
// syncpoint counts written frames, and the client acquires an image once its fence has passed.
class Nx2MovieClock {
public:
    // A longer gap between acquires is a pause, a longer lag a jump in the stream; both restart
    // the clock.
    static constexpr s64 PauseGap = 1'000'000;
    static constexpr s64 MaxCatchUp = 3'000'000;

    void Reset() {
        offset.reset();
        write_offset.reset();
    }

    // The session wrote the frame at pts (its syncpoint advanced). While the clock isn't running
    // (before the first acquire, after a pause or a loop) the first written frame starts a write
    // clock that paces the frames behind it at the movie's rate: otherwise every free image is
    // written at once and a fast client shows them all within a few of its frames, then waits
    // for the clock (a 180 Hz client with 4 images showed 4 frames in 17 ms, then nothing for
    // 44.5 ms).
    void Wrote(s64 now, u64 pts, s64 pause_end) {
        if (!Due(now, pause_end) && !write_offset) {
            write_offset = now - static_cast<s64>(pts);
        }
    }

    // The client acquired the frame at pts. The clock starts from the first acquire and runs in
    // real time from there. A frame acquired ahead of the clock doesn't move it: Kirby Air Riders
    // takes a frame before it shows it and holds it until its own clock reaches it, and following
    // those acquires ran the clock ahead of the picture, so 563 of 588 frames came out late.
    void Acquired(s64 now, u64 pts, s64 pause_end) {
        const s64 acquired_offset = now - static_cast<s64>(pts);
        if (!offset || now - last_acquire > PauseGap || pause_end > last_acquire ||
            pts < last_acquired_pts || acquired_offset - *offset > MaxCatchUp) {
            offset = acquired_offset;
        }
        last_acquire = now;
        last_acquired_pts = pts;
        write_offset.reset();
    }

    // The pts the movie's audio has reached, unless the client hasn't acquired a frame yet or for
    // a while (paused; the next acquire restarts the clock).
    std::optional<u64> Due(s64 now, s64 pause_end) const {
        if (!offset) {
            return std::nullopt;
        }
        const s64 due = now - *offset;
        if (due < 0 || now - last_acquire > PauseGap || pause_end > last_acquire) {
            return std::nullopt;
        }
        return static_cast<u64>(due);
    }

    // How long (us) the frame at pts must wait until it is within `tolerance` of the clock, or
    // nothing when it may go now (or the clock isn't running).
    // Before the client acquires, the write clock (Wrote) stands in for the movie's clock.
    std::optional<u64> HoldUs(s64 now, u64 pts, s64 pause_end, u64 tolerance) const {
        auto due = Due(now, pause_end);
        if (!due && write_offset && now >= *write_offset) {
            due = static_cast<u64>(now - *write_offset);
        }
        if (!due || pts <= *due + tolerance) {
            return std::nullopt;
        }
        return pts - *due - tolerance;
    }

private:
    std::optional<s64> offset; ///< Host time minus pts of the frame the clock started from
    std::optional<s64> write_offset; ///< The same for the first frame written before it ran
    s64 last_acquire{};
    u64 last_acquired_pts{};
};

/// Measures how fast a movie plays as the client sees it: frames handed out and how far the
/// movie's timestamps advanced, per window of host time (us), so a run can be checked against
/// the stream's rate from the log.
class Nx2MovieRateMeter {
public:
    static constexpr s64 Window = 10'000'000;

    struct Report {
        u64 frames;    ///< Handed out in the window
        s64 wall_us;   ///< Host time the window covers
        u64 movie_us;  ///< How far the timestamps advanced
    };

    /// The client was handed the frame at `pts` at host time `now`. Returns a report when the
    /// window is full, or when the timestamps went back (the stream restarted).
    std::optional<Report> Shown(s64 now, u64 pts) {
        std::optional<Report> report;
        if (m_start && pts < m_last_pts) {
            report = Finish(now);
        }
        if (!m_start) {
            m_start = now;
            m_first_pts = pts;
            m_frames = 0;
        }
        ++m_frames;
        m_last_pts = pts;
        if (now - *m_start >= Window) {
            report = Report{m_frames, now - *m_start, m_last_pts - m_first_pts};
            m_start = now;
            m_first_pts = pts;
            m_frames = 0;
        }
        return report;
    }

    /// The stream stopped: a report of the window so far, if it covers a second or more.
    std::optional<Report> Finish(s64 now) {
        std::optional<Report> report;
        if (m_start && now - *m_start >= 1'000'000 && m_frames > 0) {
            report = Report{m_frames, now - *m_start, m_last_pts - m_first_pts};
        }
        m_start.reset();
        return report;
    }

private:
    std::optional<s64> m_start;
    u64 m_first_pts{};
    u64 m_last_pts{};
    u64 m_frames{};
};

} // namespace Service
