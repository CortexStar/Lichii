# SPDX-License-Identifier: GPL-3.0-or-later

from pathlib import Path
import struct
import tempfile
import unittest

from make_graphics_warm import ENV, KEY_SIZE, runtime_records, tail_size


def environment(stage):
    return (ENV.pack(8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, stage)
            + bytes(8 + tail_size(stage)))


def graphics_record(stages=(4,), slots=(5,)):
    key = bytearray(KEY_SIZE)
    for slot in slots:
        struct.pack_into('<Q', key, slot * 8, slot + 1)
    envs = [environment(stage) for stage in stages]
    record = struct.pack('<I', len(envs)) + b''.join(envs) + key
    expected = [(stage, slot + 1, env, bytes(key))
                for stage, slot, env in zip(stages, slots, envs)]
    return record, expected


class RuntimeRecordsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / 'cache.bin'

    def read(self, payload):
        self.path.write_bytes(b'yuzucach' + struct.pack('<I', 54) + payload)
        return list(runtime_records(self.path))

    def test_complete_graphics_records_survive_compute_records(self):
        fragment, first = graphics_record()
        mesh, second = graphics_record((7, 8, 4), (1, 3, 5))
        compute = struct.pack('<I', 1) + environment(5) + bytes(24)
        self.assertEqual(self.read(fragment + compute + mesh), first + second)

    def test_every_truncated_tail_preserves_complete_prefix(self):
        prefix, expected = graphics_record()
        tails = [graphics_record((7, 8, 4), (1, 3, 5))[0],
                 struct.pack('<I', 1) + environment(5) + bytes(24)]
        for tail in tails:
            for length in range(len(tail)):
                with self.subTest(record_size=len(tail), tail_size=length):
                    self.assertEqual(self.read(prefix + tail[:length]), expected)

    def test_empty_and_impossible_record_extents_stop_parsing(self):
        prefix, expected = graphics_record()
        oversized = bytearray(environment(4))
        struct.pack_into('<Q', oversized, 0, (1 << 64) - 1)
        for tail in (bytes(4), struct.pack('<I', 1) + oversized):
            self.assertEqual(self.read(prefix + tail + prefix), expected)
        self.assertEqual(self.read(b''), [])


if __name__ == '__main__':
    unittest.main()
