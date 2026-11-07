# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the production NUP preload without starting an emulated game."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "nx2"))
from test_shader_archive import bnsh
from test_shader_containers import sharcb

BINARY = Path(sys.argv.pop(1)).resolve()
LOOKUP = None
DRIVER_CACHE = None
VERTEX_LAYOUT = None
if "--lookup-test" in sys.argv:
    position = sys.argv.index("--lookup-test")
    LOOKUP = Path(sys.argv[position + 1]).resolve()
    del sys.argv[position:position + 2]
if "--driver-cache-test" in sys.argv:
    position = sys.argv.index("--driver-cache-test")
    DRIVER_CACHE = Path(sys.argv[position + 1]).resolve()
    del sys.argv[position:position + 2]

if "--vertex-layout-test" in sys.argv:
    position = sys.argv.index("--vertex-layout-test")
    VERTEX_LAYOUT = Path(sys.argv[position + 1]).resolve()
    del sys.argv[position:position + 2]


def flat_archive(files):
    names = bytearray(b"root")
    name_ranges = []
    for name, _ in files:
        encoded = name.encode()
        name_ranges.append((len(names), len(encoded)))
        names.extend(encoded)
    count = len(files)
    pool = 48 + 64 + 24 + 80 * count + 4 * count
    toc_size = pool + len(names)
    blob = (128 + toc_size + 15) & ~15
    data = bytearray(blob)
    data[:4] = b"NUP\x01"
    struct.pack_into("<H", data, 4, 128)
    struct.pack_into("<QQ", data, 8, 128, toc_size)
    struct.pack_into("<QQI", data, 56, blob, 0, 1)
    struct.pack_into("<IIQQQQQ", data, 128, 0x01434F54, 1, 1, count, pool, len(names), 0)
    struct.pack_into("<QIIII", data, 176, 0x0400000000000000, 0, 0xffffffff, 0, 0)
    struct.pack_into("<IIII", data, 240, 0, 4, 0, count)
    for index, ((_, payload), (offset, size)) in enumerate(zip(files, name_ranges)):
        position = 264 + 80 * index
        data.extend(bytes((-len(data)) % 16))
        start = len(data)
        data.extend(payload)
        struct.pack_into("<IIQQQ", data, position, offset, size, len(payload), start, len(payload))
        data[position + 32:position + 64] = hashlib.sha256(payload).digest()
        struct.pack_into("<I", data, 264 + 80 * count + 4 * index, index + 1)
    data[128 + pool:128 + pool + len(names)] = names
    struct.pack_into("<Q", data, 64, len(data))
    data[24:56] = hashlib.sha256(data[128:128 + toc_size]).digest()
    return bytes(data)


def archive():
    shader = bytearray()
    for index, stage in enumerate((0, 4, 5, 5)):
        nops = min(index, 2)
        program = bytearray(bnsh(stage))
        start = 0x200 if stage == 5 else 0x280
        code = struct.pack("<4I", 0x7918, 0, 0, 0) * nops
        code += struct.pack("<4I", 0x794D, 0, 0x03800000, 0)
        program[start:start + len(code)] = code
        if index == 3:
            program[0x180:0x184] = b"new!"
        shader.extend(program)
    pair = bytearray(bnsh())
    pair.extend(bytes(0x200))
    struct.pack_into("<I", pair, 0x1c, len(pair))
    struct.pack_into("<Q", pair, 0x128, 0x300)
    struct.pack_into("<QQII", pair, 0x308, 0x340, 0x400, 0x100, 4)
    struct.pack_into("<I", pair, 0x400, 5 << 10)
    pair[0x280:0x290] = struct.pack("<4I", 0x794D, 0, 0x03800000, 0)
    pair[0x480:0x4a0] = struct.pack("<4I", 0x7918, 0, 0, 0) + struct.pack("<4I", 0x794D, 0, 0x03800000, 0)
    shader.extend(pair)
    members = [bytes(shader)]
    for nops, slot in ((3, 4), (4, 5)):
        program, _, _, control = sharcb(slot)
        struct.pack_into("<I", program, control + 84, (slot + 2) << 16)
        code = struct.pack("<4I", 0x7918, 0, 0, 0) * nops
        code += struct.pack("<4I", 0x794D, 0, 0x03800000, 0)
        program[0x680:0x680 + len(code)] = code
        struct.pack_into("<I", program, control + 56, 128 + len(code))
        members.append(bytes(program))
    shader = bytearray(0x100 + sum(map(len, members)))
    shader[:4] = b"SARC"
    struct.pack_into("<HHII", shader, 4, 20, 0xfeff, len(shader), 0x100)
    shader[20:24] = b"SFAT"
    struct.pack_into("<HH", shader, 24, 12, len(members))
    cursor = 0
    for index, member in enumerate(members):
        struct.pack_into("<II", shader, 40 + 16 * index, cursor, cursor + len(member))
        shader[0x100 + cursor:0x100 + cursor + len(member)] = member
        cursor += len(member)
    names = b"rootShader.sarc"
    size = 48 + 64 + 24 + 80 + 4 + len(names)
    blob = (128 + size + 15) & ~15
    data = bytearray(blob + len(shader))
    data[:4] = b"NUP\x01"
    struct.pack_into("<H", data, 4, 128)
    struct.pack_into("<QQ", data, 8, 128, size)
    struct.pack_into("<QQI", data, 56, blob, len(data), 1)
    struct.pack_into("<IIQQQQQ", data, 128, 0x01434F54, 1, 1, 1,
                     size - len(names), len(names), 0)
    struct.pack_into("<QIIII", data, 176, 0x0400000000000000, 0, 0xffffffff, 0, 0)
    struct.pack_into("<IIII", data, 240, 0, 4, 0, 1)
    struct.pack_into("<IIQQQ", data, 264, 4, len(names) - 4, len(shader), blob, len(shader))
    data[296:328] = hashlib.sha256(shader).digest()
    struct.pack_into("<I", data, 344, 1)
    data[348:348 + len(names)] = names
    data[24:56] = hashlib.sha256(data[128:128 + size]).digest()
    data[blob:] = shader
    return bytes(data)


class NupPrecompileTests(unittest.TestCase):
    @unittest.skipUnless(VERTEX_LAYOUT, "vertex layout fixture generator not supplied")
    def test_material_vertex_types_are_joined_through_shader_model(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = subprocess.run([str(VERTEX_LAYOUT), "--write-fixtures", str(root)],
                                       capture_output=True, text=True, timeout=30)
            self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
            model = (root / "synthetic_cross_materials.bfres").read_bytes()
            shader = bytearray((root / "synthetic_shader_locations.bfsha").read_bytes())
            model_offset, = struct.unpack_from("<Q", shader, 0x48)
            binary_offset, = struct.unpack_from("<Q", shader, model_offset + 168)
            program = bytearray(bnsh(0))
            program[0x280:0x290] = struct.pack("<4I", 0x794d, 0, 0x03800000, 0)
            shader[binary_offset:] = program
            struct.pack_into("<I", shader, 28, len(shader))
            source = root / "materials.nup"
            source.write_bytes(flat_archive([("model.bfres", model), ("shader.bfsha", shader)]))
            result = subprocess.run([str(BINARY), str(source), "--vertex-info"],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Known vertex types: 2:1 6:2", result.stdout)
            self.assertIn("Known vertex types: 2:1 6:3", result.stdout)
            self.assertIn("NUP material vertex candidates: 2", result.stdout)
            name_offset, = struct.unpack_from("<Q", shader, 0x38)
            shader[name_offset + 2] = ord("X")
            wrong = root / "different-model.nup"
            wrong.write_bytes(flat_archive([("model.bfres", model), ("shader.bfsha", shader)]))
            result = subprocess.run([str(BINARY), str(wrong), "--vertex-info"],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("NUP material vertex candidates: 0", result.stdout)

    @unittest.skipUnless(DRIVER_CACHE, "driver cache test executable not supplied")
    def test_embedded_driver_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "driver-fixture.nup"
            original = archive()
            source.write_bytes(original)
            source_time = source.stat().st_mtime_ns
            result = subprocess.run([str(DRIVER_CACHE), str(source)], capture_output=True,
                                    text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(source.stat().st_mtime_ns, source_time)
            self.assertEqual(sorted(path.name for path in Path(directory).iterdir()),
                             ["driver-fixture.nup", "driver-fixture.shaders.bin"])

    def test_cold_warm_corrupt_and_changed_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "synthetic.nup"
            original = archive()
            source.write_bytes(original)
            source_time = source.stat().st_mtime_ns
            cache = source.with_suffix(".shaders.bin")

            def run():
                result = subprocess.run([str(BINARY), str(source)], capture_output=True,
                                        text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                data = cache.read_bytes()
                magic, offset, size, checksum = struct.unpack_from("<8sQQ32s", data)
                self.assertEqual(magic, b"LCHNSC03")
                self.assertGreaterEqual(offset, 56)
                self.assertEqual(offset + size, len(data))
                self.assertEqual(checksum, hashlib.sha256(data[offset:]).digest())
                if LOOKUP:
                    lookup = subprocess.run([str(LOOKUP), str(source)], capture_output=True,
                                            text=True, timeout=30)
                    self.assertEqual(lookup.returncode, 0, lookup.stdout + lookup.stderr)
                return data

            cold = run()
            cached_time = cache.stat().st_mtime_ns
            self.assertEqual(run(), cold)
            self.assertEqual(cache.stat().st_mtime_ns, cached_time, "warm cache was rewritten")
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(source.stat().st_mtime_ns, source_time)

            cache.write_bytes(b"broken cache")
            run()
            changed = bytearray(original)
            changed[80] = 1  # NUP UUID changes without altering member data.
            source.write_bytes(changed)
            self.assertNotEqual(run(), cold)
            self.assertEqual(source.read_bytes(), changed)
            self.assertEqual(list(Path(directory).glob("*.pending")), [])

    def test_invalid_input_does_not_publish_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "invalid.nup"
            source.write_bytes(b"not a NUP")
            result = subprocess.run([str(BINARY), str(source)], capture_output=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(source.with_suffix(".shaders.bin").exists())

    def test_graphics_enumeration_does_not_hide_decompression_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "corrupt-archive.nup"
            data = bytearray(archive())
            blob, = struct.unpack_from("<Q", data, 56)
            toc_size, = struct.unpack_from("<Q", data, 16)
            data[blob:blob + 4] = bytes.fromhex("28b52ffd")
            data[296:328] = hashlib.sha256(data[blob:]).digest()
            data[24:56] = hashlib.sha256(data[128:128 + toc_size]).digest()
            source.write_bytes(data)
            result = subprocess.run([str(BINARY), str(source), "--graphics-info"],
                                    capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("graphics enumeration did not complete", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
