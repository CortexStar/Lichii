# SPDX-License-Identifier: GPL-3.0-or-later

import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "nx2"))
from test_shader_archive import bnsh
from test_shader_containers import sharcb

BINARY = Path(sys.argv.pop(1)).resolve()
NATIVE_EXIT = struct.pack("<4I", 0x794D, 0, 0x03800000, 0)
MAXWELL_EXIT = 0xE30000000007000F
STAGES = ("vertex", "tess-control", "tess-eval", "geometry", "fragment", "compute")


def header(stage):
    words = [0] * 20
    words[0] = ((stage + 1) << 10) | 1
    words[2] = 1 << 24
    words[3] = 1 << 24
    words[4] = 1
    if stage == 4:
        words[18] = 15
    return struct.pack("<20I", *words)


def variable_storage_classes(data):
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    cursor, classes = 5, []
    while cursor < len(words):
        size, op = words[cursor] >> 16, words[cursor] & 0xffff
        if not size or cursor + size > len(words):
            raise ValueError("invalid SPIR-V extent")
        if op == 59:  # OpVariable
            classes.append(words[cursor + 3])
        cursor += size
    return classes


def builtin_load_count(data, builtin):
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    cursor, variables, loads = 5, set(), []
    while cursor < len(words):
        size, op = words[cursor] >> 16, words[cursor] & 0xffff
        if not size or cursor + size > len(words):
            raise ValueError("invalid SPIR-V extent")
        if op == 71 and size == 4 and words[cursor + 2] == 11 and words[cursor + 3] == builtin:
            variables.add(words[cursor + 1])
        if op == 61:
            loads.append(words[cursor + 3])
        cursor += size
    return sum(pointer in variables for pointer in loads)


def linked_archive():
    data = bytearray(bnsh())
    data.extend(bytes(0x200))
    struct.pack_into("<I", data, 0x1c, len(data))
    struct.pack_into("<Q", data, 0x128, 0x300)
    struct.pack_into("<QQII", data, 0x308, 0x340, 0x400, 0x100, 104)
    struct.pack_into("<I", data, 0x15c, 104)
    struct.pack_into("<I", data, 0x400, 5 << 10)
    for offset, stage in ((0x180, 0), (0x340, 1)):
        struct.pack_into("<Q", data, offset, 0x000000c998761234)
        struct.pack_into("<I", data, offset + 56, 144)
        struct.pack_into("<II", data, offset + 64, 144, 144)
        struct.pack_into("<I", data, offset + 84, stage << 16)
    data[0x280:0x290] = NATIVE_EXIT
    data[0x480:0x490] = NATIVE_EXIT
    return data


def mesh_archive(with_task=True, task_payload=128):
    data = bytearray(bnsh())
    data.extend(bytes(0xc00))
    struct.pack_into("<I", data, 8, 0x00020401)
    struct.pack_into("<I", data, 0x1c, len(data))
    struct.pack_into("<Q", data, 0x100, 2)
    struct.pack_into("<Q", data, 0x108, 0)
    stages = [(4, 0x128, 1, 5), (8, 0x148, 7, 1)]
    if with_task:
        stages.insert(1, (7, 0x140, 6, 1))
    for index, (stage, slot, control_stage, header_stage) in enumerate(stages):
        record, control, code = 0x300 + index * 0x40, 0x500 + index * 0x100, 0x800 + index * 0x200
        struct.pack_into("<Q", data, slot, record)
        struct.pack_into("<QQII", data, record + 8, control, code, 256, 224)
        struct.pack_into("<Q", data, control, 0x000000c998761234)
        struct.pack_into("<I", data, control + 56, 144)
        struct.pack_into("<II", data, control + 64, 256, 256)
        struct.pack_into("<I", data, control + 84, control_stage << 16)
        struct.pack_into("<I", data, code, header_stage << 10)
        data[code + 128:code + 144] = NATIVE_EXIT
        if stage in (7, 8):
            struct.pack_into("<HH", data, control + 0x58, 32, task_payload if stage == 7 else 128)
        if stage == 8:
            data[control + 0x5e] = int(with_task)
            struct.pack_into("<HH", data, control + 0x60, 3, 1)
    return data


def environment(version, stage, instructions=(MAXWELL_EXIT,), formats=()):
    """Encode a synthetic captured shader for offline CLI tests."""
    code = bytearray(header(stage))
    for instruction in instructions:
        if len(code) % 32 == 0:
            code += bytes(8)
        code += struct.pack("<Q", instruction)
    counts = [len(code), 0, len(formats), 0, 0]
    if version >= 47:
        counts.append(0)
    record = struct.pack("<" + "Q" * len(counts), *counts)
    record += struct.pack("<7I", 0, 2, 80 if stage == 5 else 0, 0, len(code) - 8, 1, stage)
    record += code
    for handle, pixel_format in formats:
        record += struct.pack("<2I", handle, pixel_format)
    record += struct.pack("<4I", 1, 1, 1, 0) if stage == 5 else header(stage)
    if stage == 3:
        record += bytes(32)
    if version >= 53 and stage in (7, 8):
        record += struct.pack("<3I", 32, 64, 128) + header(0)
    return record


def pipeline_key(raw1=0, color=0, attribute=None, dynamic_type=None):
    key = bytearray(1056)
    struct.pack_into("<II", key, 48, raw1, 7 << 1)
    struct.pack_into("<f", key, 76, 1.0)
    key[56] = color
    if attribute is not None:
        struct.pack_into("<I", key, 160, 1 | (attribute << 20))
    if dynamic_type is not None:
        struct.pack_into("<Q", key, 112, dynamic_type)
    return key


def spirv_instructions(data):
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    assert words[0] == 0x07230203
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        assert size and offset + size <= len(words)
        yield opcode, words[offset + 1:offset + size]
        offset += size


def interface_type(data, storage):
    instructions = list(spirv_instructions(data))
    types = {args[0]: (op, args[1:]) for op, args in instructions if op in (21, 22, 23, 32)}
    locations = {args[0] for op, args in instructions if op == 71 and args[1:] == (30, 0)}
    for op, args in instructions:
        if op != 59 or args[1] not in locations or args[2] != storage:
            continue
        pointer = types[args[0]]
        value = types[pointer[1][1]]
        if value[0] == 23:
            value = types[value[1][0]]
        return "float" if value[0] == 22 else "signed" if value[1][1] else "unsigned"
    return None


class CliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.output = self.root / "output.spv"

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def run_cli(self, *args, code=0):
        result = subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        diagnostic = json.loads(result.stdout)
        self.assertEqual(diagnostic["schema_version"], 1)
        self.assertEqual(diagnostic["status"], "ok" if code == 0 else "error")
        return diagnostic

    def validate(self):
        validator = shutil.which("spirv-val")
        if validator:
            result = subprocess.run([validator, "--target-env", "vulkan1.2", str(self.output)],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_lower_only_unknown_stage(self):
        source = self.write("native.bin", NATIVE_EXIT)
        result = self.run_cli(source, "--lower-only", self.output)
        self.assertIsNone(result["stage"])
        self.assertEqual(self.output.read_bytes(), struct.pack("<Q", MAXWELL_EXIT))

    def test_oot_extended_compare_accumulation(self):
        for control in (0x08f25120, 0x08f05100, 0x08705130, 0x09725110,
                        0x09f05100, 0x09f45120, 0x0a745120):
            with self.subTest(control=hex(control)):
                source = self.write("oot-compare.bin", struct.pack(
                    "<4I", 0xff007c0c, 9, control, 0x000fe400) + NATIVE_EXIT)
                capture = self.write("shader.env", environment(50, 4))
                self.run_cli(source, "--environment", capture, "--cache-version", 50,
                             "--output", self.output)
                self.validate()

    def test_oot_shadowed_register_forms(self):
        instructions = (
            (0x2fe97a20, 0x01401200, 0x00410000, 0x000fe200),
            (0xfff47321, 0x000100ff, 0x00000400, 0x000fe200),
            (0x07f57a23, 0x00c01e00, 0x00010018, 0x040fe200),
            (0x14f37621, 0x01001000, 0x00010000, 0x000fe200),
            (0x5fdf7808, 0x3f800000, 0x02800000, 0x000fe200),
            (0xffda7923, 0, 0x00007800, 0x000e2800),
        )
        uniform_move = struct.pack("<4I", 0x00007c02, 0, 0x08000f00, 0)
        for instruction in instructions:
            with self.subTest(opcode=hex(instruction[0] & 0xfff)):
                source = self.write("oot-shadow.bin", uniform_move +
                                    struct.pack("<4I", *instruction) + NATIVE_EXIT)
                self.run_cli(source, "--lower-only", self.output)

    def test_oot_extended_compare_preserves_aliased_predicates(self):
        for control in (0x08f25120, 0x08f05100, 0x08705130, 0x09725110,
                        0x09f05100, 0x09f45120, 0x0a745120):
            source = self.write("oot-predicates.bin", struct.pack(
                "<4I", 0xff007c0c, 9, control, 0x000fe400) + NATIVE_EXIT)
            self.run_cli(source, "--lower-only", self.output)
            words = struct.unpack("<" + "Q" * (self.output.stat().st_size // 8),
                                  self.output.read_bytes())
            for mask in range(128):
                for value in (0, 1, 0xffffffff):
                    original = [bool(mask & (1 << i)) for i in range(7)] + [True]
                    predicates = original.copy()
                    registers = {225: value, 255: 0}
                    read_predicate = lambda encoded: predicates[encoded & 7] != bool(encoded & 8)
                    for word in words:
                        bits = lambda start, size: (word >> start) & ((1 << size) - 1)
                        if word == MAXWELL_EXIT:
                            break
                        if not read_predicate(bits(16, 4)):
                            continue
                        if word >> 48 == 0x38e8:
                            packed = sum(int(predicates[i]) << i for i in range(7))
                            selected = bits(20, 7)
                            registers[bits(0, 8)] = ((registers[bits(8, 8)] & ~selected) |
                                                      (packed & selected))
                        elif word >> 48 == 0x38f0:
                            for i in range(7):
                                if bits(20, 7) & (1 << i):
                                    predicates[i] = bool(registers[bits(8, 8)] & (1 << i))
                        elif word & 0xfff0000000000000 == 0x5b60000000000000:
                            self.assertEqual(bits(49, 3), 5)
                            self.assertEqual(bits(45, 2), 1)
                            compared = registers[bits(8, 8)] != registers[bits(20, 8)]
                            predicates[bits(3, 3)] = compared or read_predicate(bits(39, 4))
                        else:
                            self.assertEqual(word >> 48, 0x5090)
                            self.assertEqual(bits(24, 2), 0)
                            self.assertEqual(bits(45, 2), 0)
                            a = read_predicate(bits(12, 4))
                            b = read_predicate(bits(29, 4))
                            c = read_predicate(bits(39, 4))
                            for destination, result in ((bits(3, 3), a and b and c),
                                                        (bits(0, 3), not a and b and c)):
                                if destination != 7:
                                    predicates[destination] = result
                    expected = original.copy()
                    expected[(control >> 17) & 7] = ((value != 0 or original[(control >> 4) & 7])
                                                     and original[(control >> 23) & 7])
                    self.assertEqual(predicates, expected, (hex(control), mask, value))

    def test_oot_uniform_multiply_and_signed_source(self):
        for instruction in (
            (0x040474a4, 0x00003c00, 0x0f8e0206, 0x000fc600),
            (0x00037305, 0x80000013, 0x0021f100, 0x000fe200),
            (0x0e0e7819, 0x00000015, 0x00001211, 0x020fe200),
            (0x000573c4, 0, 0x00010200, 0x001e2400),
            (0x04077899, 0x18, 0x08001206, 0x002fc400),
            (0x032b7219, 0x2c, 0x0000122d, 0x000fe200),
        ):
            with self.subTest(instruction=instruction):
                source = self.write("oot-arithmetic.bin", struct.pack(
                    "<4I", *instruction) + NATIVE_EXIT)
                capture = self.write("shader.env", environment(50, 5))
                self.run_cli(source, "--environment", capture, "--cache-version", 50,
                             "--output", self.output)
                self.validate()

    def test_oot_global_reduction_forms(self):
        instructions = (
            (0xff00798e, 9, 0x0c10e704, 0x0021e800),
            (0x0c00098e, 0xfffff007, 0x0010e100, 0x0011e400),
        )
        for instruction in instructions:
            with self.subTest(instruction=instruction):
                source = self.write("oot-reduction.bin", struct.pack(
                    "<4I", *instruction) + NATIVE_EXIT)
                self.run_cli(source, "--lower-only", self.output)
                words = struct.unpack("<" + "Q" * (self.output.stat().st_size // 8),
                                      self.output.read_bytes())
                reduction = next(word for word in words if word >> 51 == 0xebf8000000000000 >> 51)
                self.assertEqual((reduction >> 20) & 7, 3 if instruction[2] == 0x0c10e704 else 0)
                self.assertEqual((reduction >> 28) & 0xfffff,
                                 0 if instruction[2] == 0x0c10e704 else 0xffff0)

    def test_oot_call_proof_accepts_discard_and_ignored_register_writes(self):
        instructions = [
            (0x00107802, 0x30, 0x00000f00, 0),
            (0x1f1f7212, 6, 0x078e3cff, 0),
            (0x00007944, 0x20, 0x03c00000, 0),
            (0x0000095b, 0, 0x03800000, 0),
            (0x0000794d, 0, 0x03800000, 0),
            (0x00027919, 0, 0x00003900, 0),
            (0x02ff7c12, 4, 0x0f80c0ff, 0),
            (0x00067202, 16, 0x00000f00, 0),
            (0x06007950, 0xffffff70, 0x03c3ffff, 0),
        ]
        source = self.write("oot-call.bin", b"".join(struct.pack("<4I", *op) for op in instructions))
        self.run_cli(source, "--lower-only", self.output)
        instructions[1] = (0x00107802, 0, 0x00000f00, 0)
        source.write_bytes(b"".join(struct.pack("<4I", *op) for op in instructions))
        result = self.run_cli(source, "--lower-only", self.output, code=1)
        self.assertEqual(result["error"]["message"], "native call linkage mode")

    def test_instruction_only_requires_stage(self):
        source = self.write("native.bin", NATIVE_EXIT)
        self.run_cli(source, "--output", self.output, code=2)
        self.assertFalse(self.output.exists())

    def test_graphics_requires_header_or_environment(self):
        source = self.write("native.bin", NATIVE_EXIT)
        result = self.run_cli(source, "--stage", "vertex", "--output", self.output, code=1)
        self.assertEqual(result["phase"], "context")
        self.assertFalse(self.output.exists())

    def test_compute_offline_assumptions(self):
        source = self.write("native.bin", NATIVE_EXIT)
        result = self.run_cli(source, "--stage", "compute", "--output", self.output)
        self.assertTrue(any("workgroup 1x1x1" in item for item in result["assumptions"]))
        self.validate()

    def test_compute_archive_control_supplies_workgroup_without_inventing_resources(self):
        source = self.write("native.bin", NATIVE_EXIT)
        control = bytearray(224)
        control[:8] = bytes.fromhex("34127698c9000000")
        struct.pack_into("<I", control, 56, len(NATIVE_EXIT))
        struct.pack_into("<II", control, 64, len(NATIVE_EXIT), len(NATIVE_EXIT))
        struct.pack_into("<IIII", control, 84, 5 << 16, 64, 1, 1)
        struct.pack_into("<H", control, 0xba, 16)
        path = self.write("native.control", control)
        result = self.run_cli(source, "--stage", "compute", "--control", path,
                              "--output", self.output)
        self.assertEqual(result["declared_workgroup"], [64, 1, 1])
        self.assertFalse(any("workgroup 1x1x1" in item for item in result["assumptions"]))
        self.assertTrue(any(op == 16 and args[1:] == (17, 64, 1, 1)
                            for op, args in spirv_instructions(self.output.read_bytes())))
        self.validate()
        struct.pack_into("<I", control, 88, 0)
        path.write_bytes(control)
        self.run_cli(source, "--stage", "compute", "--control", path,
                     "--output", self.output, code=2)

    def test_archive_control_does_not_guess_shared_allocation(self):
        code = struct.pack("<4I", 0x00087984, 0, 0xa00, 0) + NATIVE_EXIT
        source = self.write("shared.bin", code)
        control = bytearray(224)
        control[:8] = bytes.fromhex("34127698c9000000")
        struct.pack_into("<I", control, 56, len(code))
        struct.pack_into("<II", control, 64, len(code), len(code))
        struct.pack_into("<IIII", control, 84, 5 << 16, 64, 1, 1)
        struct.pack_into("<H", control, 0xba, 16)
        path = self.write("shared.control", control)
        result = self.run_cli(source, "--stage", "compute", "--control", path,
                              "--output", self.output, code=1)
        self.assertEqual(result["phase"], "context")
        self.assertIn("shared memory", result["error"]["message"])
        self.assertFalse(self.output.exists())

    def test_archive_constant_tail_is_not_executed_as_native_code(self):
        code = NATIVE_EXIT + struct.pack("<I", 0x3f800000)
        source = self.write("constant-tail.bin", code)
        control = bytearray(224)
        control[:8] = bytes.fromhex("34127698c9000000")
        struct.pack_into("<I", control, 56, len(NATIVE_EXIT))
        struct.pack_into("<II", control, 64, len(NATIVE_EXIT), len(code))
        struct.pack_into("<IIII", control, 84, 5 << 16, 32, 1, 1)
        struct.pack_into("<H", control, 0xba, 16)
        path = self.write("constant-tail.control", control)
        result = self.run_cli(source, "--stage", "compute", "--control", path,
                              "--capture-extent", "--output", self.output)
        self.assertEqual(result["archive_constant_bytes"], 4)
        self.assertEqual(result["source_bytes"], 16)
        self.validate()
        struct.pack_into("<I", control, 68, len(code) + 4)
        path.write_bytes(control)
        self.run_cli(source, "--stage", "compute", "--control", path,
                     "--output", self.output, code=2)

    def test_exact_native_header_stages(self):
        for stage, name in enumerate(STAGES[:5]):
            with self.subTest(stage=name):
                source = self.write("header.bin", header(stage) + bytes(48) + NATIVE_EXIT)
                result = self.run_cli(source, "--format", "native-header", "--output", self.output)
                self.assertEqual(result["stage"], name)
                models = [args[0] for op, args in spirv_instructions(self.output.read_bytes()) if op == 15]
                self.assertEqual(models, [stage])
                self.validate()

    def test_header_stage_mismatch(self):
        source = self.write("header.bin", header(0) + bytes(48) + NATIVE_EXIT)
        self.run_cli(source, "--format", "native-header", "--stage", "fragment", "--output", self.output, code=2)

    def test_geometry_return_before_exit_handler(self):
        instructions = (
            (0x0000734e, 0, 0, 0),
            (0x00037802, 0xe0, 0x00000f00, 0),
            (0x00007210, 3, 0x07f1e0ff, 0),
            (0xff017224, 0xff, 0x000e0401, 0),
            (0x00007357, 0, 0, 0),
            (0x00007802, 0, 0x00000f00, 0),
            (0x00017802, 0, 0x00000f00, 0),
            (0x00037802, 0, 0x00000f00, 0),
            (0x0100720c, 0xff, 0x03f03070, 0),
            (0x00087802, 0xb0, 0x00000f00, 0),
            (0x00007944, 0x10, 0x03c00000, 0),
            (0x0000794d, 0, 0x03800000, 0),
            (0x00007324, 0xff, 0x00004000, 0),
            (0x08007950, 0xffffff20, 0x03c3ffff, 0),
            (0x00ff7324, 0xff, 0, 0),
            (0x00007957, 0, 0, 0),
            (0x0000794d, 0, 0x03800000, 0),
        )
        source = self.write("geometry-call.bin", header(3) + bytes(48) +
                            b"".join(struct.pack("<4I", *op) for op in instructions))
        self.run_cli(source, "--format", "native-header", "--output", self.output)
        self.validate()
        emitted = list(spirv_instructions(self.output.read_bytes()))
        self.assertEqual(sum(op == 218 for op, _ in emitted), 1)

    def test_both_cache_versions_all_stages(self):
        """Validate ordinary stage output for each supported cache version."""
        for version in (46, 47, 48, 49, 50, 51, 52, 53, 54):
            for stage, name in enumerate(STAGES):
                with self.subTest(version=version, stage=name):
                    capture = self.write("shader.env", environment(version, stage))
                    result = self.run_cli("--environment", capture, "--cache-version", version, "--output", self.output)
                    self.assertEqual(result["stage"], name)
                    models = [args[0] for op, args in spirv_instructions(self.output.read_bytes()) if op == 15]
                    self.assertEqual(models, [stage])
                    self.validate()

    def test_v53_mesh_context_is_parsed_before_the_offline_support_check(self):
        """Distinguish intact mesh metadata from a truncated capture."""
        for stage, name in ((7, "task"), (8, "mesh")):
            capture = self.write("mesh.env", environment(53, stage))
            result = self.run_cli("--environment", capture, "--cache-version", 53,
                                  "--output", self.output, code=1)
            self.assertEqual(result["stage"], name)
            self.assertEqual(result["phase"], "context")
            self.assertIn("additional pipeline stage context", result["error"]["message"])
            capture = self.write("mesh.env", environment(53, stage)[:-1])
            result = self.run_cli("--environment", capture, "--cache-version", 53,
                                  "--output", self.output, code=2)
            self.assertEqual(result["error"]["message"], "Truncated environment record")

    def test_raw_overlay(self):
        capture = self.write("shader.env", environment(47, 4))
        source = self.write("native.bin", NATIVE_EXIT)
        result = self.run_cli(source, "--environment", capture, "--cache-version", 47, "--output", self.output)
        self.assertEqual(result["stage"], "fragment")
        self.validate()

    def test_surface_pixel_format_outside_shader_enum(self):
        for version in (46, 47, 48):
            with self.subTest(version=version):
                capture = self.write("shader.env", environment(version, 5, formats=((0, 111),)))
                self.run_cli("--environment", capture, "--cache-version", version, "--output", self.output)
                self.validate()

    def test_pixel_format_exceeds_serialized_domain(self):
        capture = self.write("shader.env", environment(47, 5, formats=((0, 256),)))
        result = self.run_cli("--environment", capture, "--cache-version", 47, "--output", self.output, code=2)
        self.assertEqual(result["error"]["message"], "Invalid texture pixel format")

    def test_truncated_records_and_trailing_data(self):
        full = environment(47, 3)
        for size in range(len(full)):
            capture = self.write("shader.env", full[:size])
            self.run_cli("--environment", capture, "--cache-version", 47, "--output", self.output, code=2)
        capture = self.write("shader.env", full + b"x")
        self.run_cli("--environment", capture, "--cache-version", 47, "--output", self.output, code=2)

    def test_invalid_lengths_versions_and_opcode(self):
        for data in (b"", NATIVE_EXIT[:-1], NATIVE_EXIT + b"x"):
            source = self.write("native.bin", data)
            self.run_cli(source, "--lower-only", self.output, code=2)
        capture = self.write("shader.env", environment(46, 5))
        self.run_cli("--environment", capture, "--cache-version", 50, "--output", self.output, code=2)
        source = self.write("native.bin", bytes(16))
        result = self.run_cli(source, "--lower-only", self.output, code=1)
        self.assertEqual(result["phase"], "lower")
        self.assertEqual(result["error"]["offset"], 0)

    def test_trap_requires_context(self):
        source = self.write("trap.bin", struct.pack("<4I", 0x795C, 0, 0, 0) + NATIVE_EXIT)
        result = self.run_cli(source, "--stage", "compute", "--lower-only", self.output, code=1)
        self.assertEqual(result["phase"], "context")

    def test_output_cannot_overwrite_input(self):
        source = self.write("native.bin", NATIVE_EXIT)
        self.run_cli(source, "--lower-only", source, code=2)
        self.assertEqual(source.read_bytes(), NATIVE_EXIT)

    def test_static_and_dynamic_vertex_types(self):
        ald = 0xEFD8000000070000 | (255 << 8) | (128 << 20) | (255 << 39)
        ast = 0xEFF0000000070000 | (255 << 8) | (112 << 20) | (255 << 39)
        capture = self.write("vertex.env", environment(47, 0, (ald, ast, MAXWELL_EXIT)))
        for dynamic in (False, True):
            for raw, expected in ((1, "float"), (2, "signed"), (3, "unsigned")):
                with self.subTest(dynamic=dynamic, expected=expected):
                    key = pipeline_key(raw1=1 << 5, dynamic_type=raw) if dynamic else pipeline_key(attribute={1: 7, 2: 3, 3: 4}[raw])
                    path = self.write("pipeline.key", key)
                    self.run_cli("--environment", capture, "--cache-version", 47, "--pipeline-key", path, "--output", self.output)
                    self.assertEqual(interface_type(self.output.read_bytes(), 1), expected)
                    self.validate()

    def test_fragment_attachment_types(self):
        capture = self.write("fragment.env", environment(47, 4))
        for color, expected in ((0xD5, "float"), (0xD8, "signed"), (0xD9, "unsigned")):
            with self.subTest(expected=expected):
                key = self.write("pipeline.key", pipeline_key(color=color))
                self.run_cli("--environment", capture, "--cache-version", 47, "--pipeline-key", key, "--output", self.output)
                self.assertEqual(interface_type(self.output.read_bytes(), 3), expected)
                self.validate()

    def test_missing_constant_value_is_context_error(self):
        mov = 0x4C98000000070000 | (15 << 39) | (1 << 34) | (0x10 << 20)
        ast = 0xEFF0000000070000 | (255 << 8) | (112 << 20) | (255 << 39)
        capture = self.write("vertex.env", environment(47, 0, (mov, ast, MAXWELL_EXIT)))
        result = self.run_cli("--environment", capture, "--cache-version", 47, "--output", self.output, code=1)
        self.assertEqual(result["phase"], "context")
        self.assertIn("constant buffer", result["error"]["message"])

    def test_static_allocation_uses_runtime_capture_boundary(self):
        source = self.write("allocation.bin", header(0) + bytes(48) + NATIVE_EXIT + bytes(256))
        result = self.run_cli(source, "--format", "native-header", "--capture-extent",
                              "--lower-only", self.output)
        self.assertEqual(result["source_bytes"], 16)
        self.assertEqual(result["allocation_bytes"], 272)

    def test_static_allocation_does_not_guess_a_missing_branch_target(self):
        branch = struct.pack("<4I", 0x7947, 0x80, 0x03800000, 0)
        source = self.write("allocation.bin", header(0) + bytes(48) + branch + NATIVE_EXIT)
        result = self.run_cli(source, "--format", "native-header", "--capture-extent",
                              "--lower-only", self.output, code=1)
        self.assertEqual(result["phase"], "context")
        self.assertIn("complete runtime capture", result["error"]["message"])

    def test_archive_info_preserves_linked_stages_and_container_identity(self):
        data = bytearray(bnsh())
        data.extend(bytes(0x200))
        struct.pack_into("<I", data, 0x1c, len(data))
        struct.pack_into("<Q", data, 0x128, 0x300)
        struct.pack_into("<QQII", data, 0x308, 0x340, 0x400, 0x100, 4)
        struct.pack_into("<I", data, 0x400, 5 << 10)
        data[0x340:0x344] = b"frag"
        combined = bytes(data) + bnsh(4)
        source = self.write("linked.bin", combined)
        result = self.run_cli("--archive-info", source)
        self.assertEqual(result["vertex_fragment_variations"], 1)
        groups = result["linked_variations"]
        self.assertEqual(len(groups), 2)
        self.assertEqual([g["container_offset"] for g in groups], [0, len(data)])
        self.assertEqual([g["variation"] for g in groups], [0, 0])
        self.assertEqual(set(groups[0]["stages"]), {"vertex", "fragment"})
        self.assertEqual(set(groups[1]["stages"]), {"fragment"})
        frag = groups[0]["stages"]["fragment"]
        self.assertEqual(combined[frag["control_offset"]:frag["control_offset"] +
                                  frag["control_size"]], b"frag")

    def test_archive_info_does_not_invent_bahs_linkage(self):
        source = self.write("unlinked.sharcb", sharcb()[0])
        result = self.run_cli("--archive-info", source)
        self.assertEqual(result["linked_variations"], [])
        self.assertEqual(result["unlinked_stage_records"], 2)

    def test_graphics_probe_emits_linked_modules_under_explicit_contract(self):
        source = self.write("linked.bnsh", linked_archive())
        output = self.output.parent / "linked-output"
        result = self.run_cli("--graphics-probe", source, output)
        self.assertEqual(result["graphics_probe"]["compiled_groups"], 1)
        report = json.loads((output / "report.json").read_text())
        self.assertIn("Not a runtime or final-driver-pipeline cache", report["contract"])
        self.assertEqual(report["results"][0]["status"], "compiled_under_contract")
        self.assertEqual(len(report["results"][0]["modules"]), 2)
        for name in report["results"][0]["modules"]:
            self.output.write_bytes((output / name).read_bytes())
            self.validate()

    def test_graphics_probe_defers_missing_archive_context(self):
        data = linked_archive()
        data[0x180:0x188] = bytes(8)
        source = self.write("missing-control.bnsh", data)
        output = self.output.parent / "missing-output"
        result = self.run_cli("--graphics-probe", source, output)
        self.assertEqual(result["graphics_probe"]["compiled_groups"], 0)
        report = json.loads((output / "report.json").read_text())
        self.assertIn("extents are unavailable", report["results"][0]["reason"])
        self.assertEqual(list(output.glob("*.spv")), [])

    def test_graphics_probe_uses_archive_mesh_and_task_dimensions(self):
        source = self.write("mesh.bnsh", mesh_archive())
        output = self.output.parent / "mesh-output"
        result = self.run_cli("--graphics-probe", source, output)
        self.assertEqual(result["graphics_probe"]["compiled_groups"], 1)
        report = json.loads((output / "report.json").read_text())
        group = report["results"][0]
        self.assertEqual(group["mesh_contract"]["threads"], 32)
        self.assertEqual(group["mesh_contract"]["payload_bytes"], 128)
        self.assertIn("not inferred", group["mesh_contract"]["topology"])
        self.assertEqual(len(group["modules"]), 3)
        for name in group["modules"]:
            self.output.write_bytes((output / name).read_bytes())
            if name.endswith(("-task.spv", "-mesh.spv")):
                self.assertIn(5402, variable_storage_classes(self.output.read_bytes()))
            self.validate()

    def test_graphics_probe_rejects_mismatched_task_payload(self):
        source = self.write("mesh.bnsh", mesh_archive(task_payload=256))
        output = self.output.parent / "mesh-mismatch-output"
        result = self.run_cli("--graphics-probe", source, output)
        self.assertEqual(result["graphics_probe"]["compiled_groups"], 0)
        report = json.loads((output / "report.json").read_text())
        self.assertIn("launch layout", report["results"][0]["reason"])

    def test_graphics_probe_mesh_without_task_does_not_declare_payload(self):
        source = self.write("mesh-no-task.bnsh", mesh_archive(with_task=False))
        output = self.output.parent / "mesh-no-task-output"
        result = self.run_cli("--graphics-probe", source, output)
        self.assertEqual(result["graphics_probe"]["compiled_groups"], 1)
        report = json.loads((output / "report.json").read_text())
        mesh_path = next(output / p for p in report["results"][0]["modules"] if p.endswith("-mesh.spv"))
        data = mesh_path.read_bytes()
        self.assertNotIn(5402, variable_storage_classes(data))  # TaskPayloadWorkgroupEXT
        self.output.write_bytes(data)
        self.validate()

    def test_native_isberd_distinguishes_workgroup_from_absent_input(self):
        modules = []
        for skew in (0, 1):
            data = mesh_archive(with_task=False)
            read = 0x7923 | (255 << 24) | (1 << 75) | (3 << 76) | (skew << 78)
            write = 0x7927 | (255 << 24) | (1 << 75) | (3 << 76) | (1 << 79)
            data[0xa80:0xab0] = read.to_bytes(16, "little") + write.to_bytes(16, "little") + NATIVE_EXIT
            struct.pack_into("<I", data, 0x600 + 56, 176)
            source = self.write(f"isberd-{skew}.bnsh", data)
            output = self.output.parent / f"isberd-{skew}-output"
            result = self.run_cli("--graphics-probe", source, output)
            self.assertEqual(result["graphics_probe"]["compiled_groups"], 1)
            report = json.loads((output / "report.json").read_text())
            path = next(output / p for p in report["results"][0]["modules"] if p.endswith("-mesh.spv"))
            modules.append(path.read_bytes())
            self.output.write_bytes(modules[-1])
            self.validate()
        self.assertNotEqual(modules[0], modules[1], "native ISBERD mode was discarded")
        self.assertEqual(builtin_load_count(modules[0], 26), 0)  # WorkgroupId
        self.assertGreater(builtin_load_count(modules[1], 26), 0)

    def test_graphics_probe_texture_contract_is_explicit(self):
        source = self.write("linked.bnsh", linked_archive())
        output = self.output.parent / "texture-contract-output"
        self.run_cli("--graphics-probe", source, output, "--float-textures")
        report = json.loads((output / "report.json").read_text())
        self.assertIn("raw handles are not read or invented", report["texture_contract"])
        self.assertEqual(report["results"][0]["texture_dependencies"], [[], []])

    def test_graphics_probe_rejects_unknown_contract_option(self):
        source = self.write("linked.bnsh", linked_archive())
        output = self.output.parent / "invalid-contract-output"
        self.run_cli("--graphics-probe", source, output, "--unknown-contract", code=2)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
