#!/usr/bin/env python3
"""Verify the recorded return-window statistics using Python's standard library."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parent


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low, high = math.floor(position), math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=ROOT)
    root = parser.parse_args().data_dir
    expected = json.loads((root / "results.json").read_text())
    groups = {}
    with (root / "return-intervals.csv").open(newline="") as source:
        for row in csv.DictReader(source):
            groups.setdefault(row["run"], []).append(row)
    assert set(groups) == {row["run"] for row in expected["runs"]}
    summaries = {}
    for recorded in expected["runs"]:
        run = recorded["run"]
        rows = groups[run]
        assert len(rows) == recorded["frames"] == 600, run
        first = int(rows[0]["present"])
        for index, row in enumerate(rows, 1):
            assert row["mode"] == recorded["mode"], run
            assert int(row["interval_index"]) == index, run
            assert int(row["present"]) == first + index - 1, run
            assert int(row["interval_ns"]) > 0, run
        values = [int(row["interval_ns"]) / 1e6 for row in rows]
        computed = {
            "fps": 1000 / statistics.mean(values),
            "p50_ms": percentile(values, 0.5),
            "p95_ms": percentile(values, 0.95),
            "p99_ms": percentile(values, 0.99),
            "max_ms": max(values),
            "over_100ms": sum(value > 100 for value in values),
            "over_500ms": sum(value > 500 for value in values),
            "total_over_100ms": sum(value for value in values if value > 100),
        }
        for key, value in computed.items():
            assert math.isclose(value, recorded[key], rel_tol=1e-10, abs_tol=1e-7), (run, key)
        summaries[run] = computed
        print(f"{run}: p50={computed['p50_ms']:.2f} ms "
              f"p99={computed['p99_ms']:.2f} ms max={computed['max_ms']:.2f} ms")
    for pair in expected["pairs"]:
        normal = summaries[pair["normal"]]["p99_ms"]
        keep = summaries[pair["keep"]]["p99_ms"]
        reduction = 100 * (normal - keep) / normal
        assert math.isclose(normal, pair["normal_p99_ms"], abs_tol=1e-7)
        assert math.isclose(keep, pair["keep_p99_ms"], abs_tol=1e-7)
        assert math.isclose(reduction, pair["p99_reduction_percent"], abs_tol=1e-7)
        print(f"{pair['normal']}/{pair['keep']}: p99 reduction={reduction:.2f}%")
    for mode in ("normal", "keep"):
        for key, value in expected[f"{mode}_run_medians"].items():
            computed = statistics.median(summaries[row["run"]][key]
                                         for row in expected["runs"] if row["mode"] == mode)
            assert math.isclose(computed, value, abs_tol=1e-7), (mode, key)
    print(f"Verified {len(groups)} windows, {sum(map(len, groups.values()))} intervals, "
          f"{len(expected['pairs'])} pairs and run medians.")


if __name__ == "__main__":
    main()
