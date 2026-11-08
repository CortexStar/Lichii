#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay a frame-timed input sequence and save guest-present stutter measurements.

Run only in an isolated test session with the controller idle. The sequence file is
the JSON params object for input_sequence (steps and optional timeout_s). Restore
identical saves and starting caches for each comparison. This does not reset them.
"""

import argparse
import importlib.util
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sequence", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--server", default="127.0.0.1:6545")
    parser.add_argument("--label", default="")
    args = parser.parse_args()
    sequence = json.loads(args.sequence.read_text())
    frames = sum(step["frames"] for step in sequence["steps"])
    if not 1 <= frames <= 4000:
        parser.error("sequence must contain 1..4000 presents (4096-slot measurement ring)")

    spec = importlib.util.spec_from_file_location("debug_mcp", Path(__file__).with_name("debug_mcp.py"))
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    emulator = adapter.Emulator(args.server)
    try:
        status = emulator.call("status", {"fps_window_ms": 0}, 5)
        replay = emulator.call("input_sequence", sequence,
                               adapter.default_timeout("input_sequence", sequence))
        measured = emulator.call("frame_times", {
            "since_present": replay["first_present"] - 1,
            "include_intervals": True,
        }, 5)
    finally:
        emulator.close()

    intervals = measured["intervals_ms"]
    result = {"label": args.label, "status": status, "sequence": sequence,
              "replay": replay, "measurement": measured,
              "over_ms": {str(limit): sum(value > limit for value in intervals)
                          for limit in (33.333333, 50, 100, 250, 500)},
              "excess_over_60hz_ms": sum(max(0, value - 1000 / 60) for value in intervals)}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if measured["truncated"] or len(intervals) < frames - 1:
        raise SystemExit("Incomplete measurement; saved for diagnosis, do not compare it.")
    print(json.dumps({key: result[key] for key in ("label", "over_ms", "excess_over_60hz_ms")}))
    print(json.dumps({key: measured[key] for key in ("frames", "p50_ms", "p99_ms", "max_ms")}))


if __name__ == "__main__":
    main()
