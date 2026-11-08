#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import base64
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import debug_mcp
import measure_stutters
from measure_stutters import read_window, split_sequence, summarize


class LongMeasurementTest(unittest.TestCase):
    def test_long_neutral_wait_preserves_all_frames_and_timeout(self):
        chunks = list(split_sequence({"steps": [{"frames": 5407}], "timeout_s": 300}))
        self.assertEqual([sum(s["frames"] for s in c["steps"]) for c in chunks],
                         [1800, 1800, 1800, 7])
        self.assertTrue(all(c["timeout_s"] == 300 for c in chunks))

    def test_button_press_is_not_split_across_requests(self):
        chunks = list(split_sequence({"steps": [
            {"frames": 1798}, {"buttons": ["A"], "frames": 6}, {"frames": 1800}]}))
        self.assertEqual(chunks[0]["steps"], [{"frames": 1798}])
        self.assertEqual(chunks[1]["steps"][0], {"buttons": ["A"], "frames": 6})
        self.assertEqual(chunks[2]["steps"], [{"frames": 6}])

    def test_step_count_respects_server_limit(self):
        chunks = list(split_sequence({"steps": [{"frames": 1}] * 1801}))
        self.assertEqual([len(c["steps"]) for c in chunks], [1000, 801])

    def test_invalid_or_oversized_hold_is_rejected_before_replay(self):
        for step in [{"frames": 0}, {"frames": -1}, {"frames": True},
                     {"frames": 1801, "left_stick": [1, 0]}]:
            with self.assertRaises(ValueError):
                list(split_sequence({"steps": [step]}))

    def test_statistics_use_the_full_series_including_boundary_stall(self):
        result = summarize([10.0] * 100 + [500.0] + [20.0] * 99)
        self.assertEqual(result["frames"], 200)
        self.assertEqual(result["duration_ms"], 3480.0)
        self.assertEqual(result["p50_ms"], 15.0)
        self.assertEqual(result["p99_ms"], 20.0)
        self.assertEqual(result["max_ms"], 500.0)
        self.assertEqual(result["stutters"], 1)

    def test_uncertain_disconnect_does_not_replay_input(self):
        connection = Mock()
        connection.recv.return_value = b""
        with patch.object(debug_mcp.socket, "create_connection", return_value=connection) as create:
            emulator = debug_mcp.Emulator("127.0.0.1:6547")
            with self.assertRaises(debug_mcp.EmulatorError):
                emulator.call("input_sequence", {"steps": [{"frames": 1}]}, 5,
                              retry_disconnect=False)
            self.assertEqual(create.call_count, 1)
            self.assertEqual(connection.sendall.call_count, 1)

    def test_pending_timestamp_retries_same_cursor_without_input(self):
        emulator = Mock()
        incomplete = {"before_present": 10, "after_present": 13, "frames": 1,
                      "intervals_ms": [16.0], "truncated": False}
        complete = {"before_present": 10, "after_present": 14, "frames": 3,
                    "intervals_ms": [16.0, 500.0, 16.0], "truncated": False}
        emulator.call.side_effect = [incomplete, complete]
        measured, valid = read_window(emulator, 10)
        self.assertTrue(valid)
        self.assertEqual(measured, complete)
        for call in emulator.call.call_args_list:
            self.assertEqual(call.args[0], "frame_times")
            self.assertEqual(call.args[1]["since_present"], 10)

    def test_truncated_window_is_rejected_without_retry(self):
        emulator = Mock()
        emulator.call.return_value = {"before_present": 0, "after_present": 5000,
                                      "frames": 1, "intervals_ms": [20.0], "truncated": True}
        _, valid = read_window(emulator, 0)
        self.assertFalse(valid)
        self.assertEqual(emulator.call.call_count, 1)

    def test_full_runner_retains_boundary_and_endpoint_intervals_once(self):
        class Emulator:
            counter = 10

            def call(self, method, params, timeout, retry_disconnect=True):
                if method == "status":
                    return {"presents": self.counter}
                if method == "input_sequence":
                    first = self.counter + 1
                    frames = sum(step["frames"] for step in params["steps"])
                    self.counter = first + frames
                    return {"first_present": first, "frames": frames}
                if method == "screenshot":
                    self.counter += 1
                    return {"png_base64": base64.b64encode(b"test image").decode()}
                before = params["since_present"]
                intervals = [500.0 if frame == 15 else 16.0
                             for frame in range(before + 2, self.counter + 1)]
                return {"before_present": before, "after_present": self.counter,
                        "frames": len(intervals), "intervals_ms": intervals, "truncated": False}

            def close(self):
                pass

        adapter = SimpleNamespace(Emulator=lambda address: Emulator(),
                                  default_timeout=debug_mcp.default_timeout)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sequence, output, image = root / "sequence.json", root / "result.json", root / "end.png"
            sequence.write_text(json.dumps({"steps": [{"buttons": ["A"], "frames": 1},
                                                       {"frames": 4}]}))
            with patch("sys.argv", ["measure_stutters", str(sequence), str(output),
                                    "--chunk-frames", "3", "--endpoint-image", str(image)]), \
                 patch.object(measure_stutters.importlib.util, "spec_from_file_location"), \
                 patch.object(measure_stutters.importlib.util, "module_from_spec", return_value=adapter), \
                 patch("builtins.print"):
                measure_stutters.main()
            result = json.loads(output.read_text())
            self.assertEqual(result["replay"]["frames"], 5)
            self.assertEqual(result["measurement"]["frames"], 7)
            self.assertEqual(result["measurement"]["intervals_ms"],
                             [16.0, 16.0, 16.0, 500.0, 16.0, 16.0, 16.0])
            self.assertEqual(result["over_ms"]["100"], 1)
            self.assertTrue(result["endpoint_capture_included"])
            self.assertEqual(image.read_bytes(), b"test image")


if __name__ == "__main__":
    unittest.main()
