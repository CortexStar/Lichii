#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import json
from pathlib import Path
import tempfile
import unittest

from frame_time_metrics import read_intervals, summarize


class FrameMetricsTests(unittest.TestCase):
    def test_throughput_uses_total_time_not_average_instantaneous_fps(self):
        result = summarize([10, 30])
        self.assertEqual(result['average_fps'], 50)
        self.assertEqual(result['median_ms'], 20)
        self.assertEqual(result['p95_ms'], 29)
        self.assertEqual(result['p99_ms'], 29.8)

    def test_thresholds_are_strict_and_budget_is_explicit(self):
        result = summarize([1000/60, 33.3, 50, 100, 100.01])
        self.assertEqual(result['within_60fps_budget_share'], .2)
        self.assertEqual(result['over_33_3_ms'], 3)
        self.assertEqual(result['over_50_ms'], 2)
        self.assertEqual(result['over_100_ms'], 1)

    def test_invalid_or_empty_windows_fail(self):
        for values in ([], [0], [-1], [True], ['16.7'], [float('nan')], [float('inf')]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                summarize(values)

    def test_recent_only_json_cannot_impersonate_complete_window(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            path.write_text(json.dumps({'frames': 600, 'intervals_ms': [16.7]*120}))
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_truncated_json_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            path.write_text(json.dumps({'truncated': True, 'intervals_ms': [16.7]}))
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_input_contaminated_window_cannot_enter_comparison(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            path.write_text(json.dumps({'valid_for_comparison': False,
                                        'frames': 2, 'intervals_ms': [10, 20]}))
            with self.assertRaisesRegex(ValueError, 'excluded'):
                read_intervals(path)

    def test_missing_or_conflicting_counts_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            for metadata in ({}, {'frames': 2, 'summary': {'intervals': 600}}):
                with self.subTest(metadata=metadata):
                    path.write_text(json.dumps({**metadata, 'intervals_ms': [10, 20]}))
                    with self.assertRaises(ValueError):
                        read_intervals(path)
            path.write_text(json.dumps({'frames': True, 'intervals_ms': [10]}))
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_tsv_bounds_exclude_menus_and_require_complete_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.tsv'
            path.write_text('# recorder\nframe\tinterval_ms\n1\t1000\n2\t10\n3\t20\n4\t1000\n')
            self.assertEqual(read_intervals(path, 2, 4), [10, 20])
            with self.assertRaises(ValueError):
                read_intervals(path, 2, 6)

    def test_tsv_gaps_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.tsv'
            path.write_text('frame\tinterval_ms\n2\t10\n4\t20\n')
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_native_csv_nanoseconds_and_bounds(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'presents.csv'
            path.write_text('frame,return_ns,interval_ns\n1,100,0\n2,10000100,10000000\n3,30000100,20000000\n')
            self.assertEqual(read_intervals(path, 2, 4), [10, 20])
            with self.assertRaises(ValueError):
                summarize(read_intervals(path))

    def test_audited_native_csv_rejects_manual_interference(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'presents.csv'
            path.write_text('frame,interval_ns,input_interference_count,input_audit_enabled\n'
                            '1,0,0,1\n2,10000000,0,1\n3,20000000,1,1\n')
            with self.assertRaisesRegex(ValueError, 'excluded'):
                read_intervals(path, 2, 4)

    def test_audited_native_csv_keeps_quiet_stalls_and_requires_boundary(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'presents.csv'
            header = 'frame,interval_ns,input_interference_count,input_audit_enabled\n'
            path.write_text(header + '1,0,7,1\n2,10000000,7,1\n3,200000000,7,1\n')
            self.assertEqual(read_intervals(path, 2, 4), [10, 200])
            path.write_text(header + '2,10000000,7,1\n3,200000000,7,1\n')
            with self.assertRaisesRegex(ValueError, 'complete'):
                read_intervals(path, 2, 4)


if __name__ == '__main__':
    unittest.main()
