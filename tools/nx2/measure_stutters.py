#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay a frame-timed input sequence and save guest-present stutter measurements.

Run only in an isolated test session with the controller idle. The sequence file is
the JSON params object for input_sequence (steps and optional timeout_s). Restore
identical saves and starting caches for each comparison. This does not reset them.
"""

import argparse
import base64
import importlib.util
import json
import math
import time
from pathlib import Path


def split_sequence(sequence, limit=1800):
    """Keep each request below the present ring's capacity, preserving input steps."""
    if limit < 1:
        raise ValueError("chunk limit must be positive")
    chunk = []
    remaining = limit
    for step in sequence["steps"]:
        frames = step["frames"]
        if not isinstance(frames, int) or isinstance(frames, bool) or frames <= 0:
            raise ValueError("each step must contain a positive integer frame count")
        while frames:
            if len(chunk) == 1000:
                yield {**sequence, "steps": chunk}
                chunk, remaining = [], limit
            # Avoid releasing a held input at a request boundary. Only neutral waits can split.
            active = (step.get("buttons") or any(step.get("left_stick", [])) or
                      any(step.get("right_stick", [])))
            if active and frames > limit:
                raise ValueError(f"held input exceeds the {limit}-present chunk limit")
            if active and frames > remaining:
                yield {**sequence, "steps": chunk}
                chunk, remaining = [], limit
            take = min(frames, remaining)
            chunk.append({**step, "frames": take})
            frames -= take
            remaining -= take
            if remaining == 0:
                yield {**sequence, "steps": chunk}
                chunk, remaining = [], limit
    if chunk:
        yield {**sequence, "steps": chunk}


def summarize(intervals):
    ordered = sorted(intervals)
    def percentile(fraction):
        index = fraction * (len(ordered) - 1)
        low = int(index)
        high = min(low + 1, len(ordered) - 1)
        return ordered[low] + (ordered[high] - ordered[low]) * (index - low)

    mean = sum(intervals) / len(intervals)
    median = percentile(.5)
    return {"frames": len(intervals), "duration_ms": sum(intervals),
            "mean_ms": mean, "fps": 1000 / mean if mean > 0 else 0,
            "p50_ms": median, "p90_ms": percentile(.9), "p99_ms": percentile(.99),
            "min_ms": ordered[0], "max_ms": ordered[-1],
            "stutters": sum(value > 2 * median for value in intervals),
            "intervals_ms": intervals, "truncated": False}


def read_window(emulator, before):
    for attempt in range(3):
        measured = emulator.call("frame_times", {
            "since_present": before, "include_intervals": True,
        }, 5)
        expected = measured["after_present"] - before - 1
        valid = (not measured["truncated"] and measured["before_present"] == before and
                 measured["frames"] == len(measured["intervals_ms"]) == expected and
                 expected > 0 and all(math.isfinite(value) and value >= 0
                                      for value in measured["intervals_ms"]))
        if valid or measured["truncated"]:
            return measured, valid
        # A present counter may become visible just before its timestamp store.
        # Reread the same window; never replay input to recover a sample.
    return measured, valid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sequence", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--server", default="127.0.0.1:6545")
    parser.add_argument("--label", default="")
    parser.add_argument("--chunk-frames", type=int, default=1800,
                        help="presents per sample, 1..1800 (default 1800)")
    parser.add_argument("--stop-file", type=Path,
                        help="end after a sample when this previously absent checkpoint file appears")
    parser.add_argument("--endpoint-image", type=Path,
                        help="save one final screenshot for endpoint verification; its cost is measured")
    parser.add_argument("--max-seconds", type=float, default=1800,
                        help="overall wall-time limit, checked between chunks (default 1800)")
    args = parser.parse_args()
    if not 1 <= args.chunk_frames <= 1800:
        parser.error("chunk-frames must be 1..1800")
    if not math.isfinite(args.max_seconds) or args.max_seconds <= 0:
        parser.error("max-seconds must be finite and positive")
    if args.stop_file and args.stop_file.exists():
        parser.error("stop-file already exists; restore the intended starting state")
    sequence = json.loads(args.sequence.read_text())
    frames = sum(step["frames"] for step in sequence["steps"])
    if not 1 <= frames <= 36000:
        parser.error("sequence must contain 1..36000 presents")
    try:
        sequences = list(split_sequence(sequence, args.chunk_frames))
    except ValueError as error:
        parser.error(str(error))

    spec = importlib.util.spec_from_file_location("debug_mcp", Path(__file__).with_name("debug_mcp.py"))
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    emulator = adapter.Emulator(args.server)
    chunks, intervals = [], []
    after = None
    checkpoint_reached = False
    try:
        status = emulator.call("status", {"fps_window_ms": 0}, 5)
        started = time.monotonic()
        for index, part in enumerate(sequences):
            if time.monotonic() - started >= args.max_seconds:
                raise SystemExit("Overall measurement time limit exceeded; run incomplete.")
            timeout = part.get("timeout_s", adapter.default_timeout("input_sequence", part)) + 5
            replay = emulator.call("input_sequence", part, timeout, retry_disconnect=False)
            # Reinclude the previous final timestamp, retaining the interval across requests.
            before = (replay["first_present"] if after is None else after) - 1
            measured, valid = read_window(emulator, before)
            chunks.append({"replay": replay, "before_present": before,
                           "after_present": measured["after_present"], "valid": valid})
            if not valid:
                args.output.write_text(json.dumps({"label": args.label, "valid": False,
                    "chunks": chunks, "failed_measurement": measured}, indent=2) + "\n")
                raise SystemExit("Incomplete measurement; saved for diagnosis, do not compare it.")
            intervals.extend(measured["intervals_ms"])
            after = measured["after_present"]
            print(f"Measured chunk {index + 1}/{len(sequences)}: {len(intervals)} intervals, "
                  f"{sum(value > 100 for value in intervals)} over 100 ms, "
                  f"worst {max(intervals):.1f} ms",
                  flush=True)
            if args.stop_file and args.stop_file.exists():
                checkpoint_reached = True
                break
        if args.endpoint_image:
            shot = emulator.call("screenshot", {}, 10, retry_disconnect=False)
            args.endpoint_image.write_bytes(base64.b64decode(shot["png_base64"]))
            before = after - 1
            measured, valid = read_window(emulator, before)
            if not valid:
                raise SystemExit("Incomplete endpoint measurement; do not compare this run.")
            intervals.extend(measured["intervals_ms"])
            after = measured["after_present"]
            chunks.append({"replay": {"frames": 0}, "kind": "endpoint_capture",
                           "before_present": before, "after_present": after, "valid": True})
        elapsed = time.monotonic() - started
    finally:
        emulator.close()

    measured = summarize(intervals)
    if len(intervals) != after - chunks[0]["replay"]["first_present"]:
        raise SystemExit("Non-contiguous measurement; do not compare it.")
    measured.update(before_present=chunks[0]["before_present"], after_present=after)
    replay = {"frames": sum(chunk["replay"]["frames"] for chunk in chunks),
              "requested_frames": frames, "steps": len(sequence["steps"]), "elapsed_s": elapsed,
              "first_present": chunks[0]["replay"]["first_present"]}
    result = {"label": args.label, "status": status, "sequence": sequence,
              "replay": replay, "chunks": chunks, "measurement": measured,
              "over_ms": {str(limit): sum(value > limit for value in intervals)
                          for limit in (33.333333, 50, 100, 250, 500)},
              "excess_over_60hz_ms": sum(max(0, value - 1000 / 60) for value in intervals)}
    if args.stop_file:
        result["checkpoint"] = {"path": str(args.stop_file), "reached": checkpoint_reached,
                                "note": "File appearance is a trigger; verify the game endpoint separately."}
    if args.endpoint_image:
        result["endpoint_image"] = str(args.endpoint_image)
        result["endpoint_capture_included"] = True
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    if args.stop_file and not checkpoint_reached:
        raise SystemExit("Checkpoint not reached; saved partial run, do not compare as complete.")
    print(json.dumps({key: result[key] for key in ("label", "over_ms", "excess_over_60hz_ms")}))
    print(json.dumps({key: measured[key] for key in ("frames", "p50_ms", "p99_ms", "max_ms")}))


if __name__ == "__main__":
    main()
