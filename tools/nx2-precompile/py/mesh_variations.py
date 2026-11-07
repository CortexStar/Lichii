"""Mesh pipelines' shader combinations, from the variation tables of the game's BNSH archives.

nxnative's pair tables hold vertex/pixel pairs only. A BNSH variation's program also names its task
and mesh shaders (program +0x40, +0x48; nxnative shader_split.py bnsh_shaders), so the (task, mesh,
pixel) combinations a mesh draw can use are read here the same way.

usage: <venv python> mesh_variations.py <RomFS.img> <inventory.json> <out.json>
Reads only; needs the zstandard module.
"""
import collections
import hashlib
import json
import struct
import sys

import zstandard

HEAD = struct.Struct('<8sQQQ')
ENT = struct.Struct('<QQIHBB')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
ARCHIVES = ['Shader/Model.Product.110.product.Nin_Ounce_NVN2.bfsha.zs',
            'Shader/Foliage.Product.110.product.Nin_Ounce_NVN2.bfsha.zs',
            'Shader/ApplicationPackage.Nin_Ounce_NVN2.release.sarc.zs']


class Image:
    """nxnative's DMROMFS1 image: header, then entries (data offset, size or child range, name)."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        magic, off, size, count = HEAD.unpack(self.f.read(HEAD.size))
        assert magic == b'DMROMFS1', magic
        self.f.seek(off)
        self.index = self.f.read(size)
        self.pool = count * ENT.size

    def entry(self, k):
        return ENT.unpack_from(self.index, k * ENT.size)

    def name(self, k):
        _, _, no, nl, _, _ = self.entry(k)
        return self.index[self.pool + no:self.pool + no + nl].decode()

    def read(self, relative):
        k = 0
        for part in relative.split('/'):
            a, b, _, _, t, _ = self.entry(k)
            assert not t
            for j in range(a, a + b):
                if self.name(j) == part:
                    k = j
                    break
            else:
                raise FileNotFoundError(relative)
        a, b, _, _, t, _ = self.entry(k)
        assert t
        self.f.seek(a)
        return self.f.read(b)


DICTS = {}


def load_dictionaries(img):
    """Pack/ZsDic.pack.zs: a plain-zstd SARC of .zsdic zstd dictionaries (magic EC30A437, ID)."""
    data = zstandard.ZstdDecompressor().decompressobj().decompress(img.read('Pack/ZsDic.pack.zs'))
    for name, blob in sarc_members(data):
        if blob[:4] == bytes.fromhex('37a430ec'):
            DICTS[struct.unpack_from('<I', blob, 4)[0]] = zstandard.ZstdCompressionDict(blob)


def unzstd(data):
    did = zstandard.get_frame_parameters(data).dict_id
    if did:
        return zstandard.ZstdDecompressor(dict_data=DICTS[did]).decompressobj().decompress(data)
    return zstandard.ZstdDecompressor().decompressobj().decompress(data)


def sarc_members(d):
    assert d[:4] == b'SARC'
    data_off = struct.unpack_from('<I', d, 0x0C)[0]
    sfat = 0x14
    count = struct.unpack_from('<H', d, sfat + 6)[0]
    nodes = sfat + 0x0C
    sfnt = nodes + 16 * count
    names = sfnt + 8
    for i in range(count):
        _, attr, begin, end = struct.unpack_from('<IIII', d, nodes + 16 * i)
        name_off = (attr & 0xFFFF) * 4
        name = d[names + name_off:d.index(b'\0', names + name_off)].decode()
        yield name, d[data_off + begin:data_off + end]


def skey(ctl, code):
    c = ctl[:0x48]
    c = c[:0x30] + bytes(4) + c[0x34:]
    return hashlib.sha256(c + code).hexdigest()[:16]


def program_stage(d, base, sc):
    if not sc or base + sc + 0x20 > len(d):
        return None
    pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', d, base + sc + 8)
    if not 0x48 <= nctl < 0x10000 or base + pctl + nctl > len(d) or base + pcode + ncode > len(d):
        return None
    ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
    if ctl[:4] != bytes.fromhex('34127698'):
        return None
    return skey(ctl, code)


def bnsh_variations(d):
    """(label, {stage: key}) per variation of every BNSH in d."""
    pos = d.find(b'BNSH')
    while pos >= 0:
        if d[pos + 0x60:pos + 0x64] == b'grsc':
            nvar, pvar = struct.unpack_from('<IQ', d, pos + 0x7C)
            for v in range(nvar):
                if pos + pvar + 0x40 * v + 0x18 > len(d):
                    break
                prog = struct.unpack_from('<Q', d, pos + pvar + 0x40 * v + 0x10)[0]
                if not prog:
                    continue
                stages = {}
                for si, st in enumerate(STAGES):
                    k = program_stage(d, pos, struct.unpack_from('<Q', d, pos + prog + 8 + 8 * si)[0])
                    if k:
                        stages[st] = k
                for off, st in ((0x40, 'ts'), (0x48, 'ms')):
                    if pos + prog + off + 8 <= len(d):
                        k = program_stage(d, pos, struct.unpack_from('<Q', d, pos + prog + off)[0])
                        if k:
                            stages[st] = k
                yield '%#x#var%d' % (pos, v), stages
        pos = d.find(b'BNSH', pos + 4)


image_path, inv_path, out_path = sys.argv[1:4]
inv = json.load(open(inv_path))
hash_of = {}
for r in inv:
    hash_of.setdefault(r['skey'], r['code_hash'])
img = Image(image_path)
load_dictionaries(img)
print("zstd dictionaries", sorted(DICTS))
combos = set()
counts = collections.Counter()
for path in ARCHIVES:
    data = unzstd(img.read(path))
    blobs = [(path, data)] if data[:4] != b'SARC' else [(path + '/' + n, m) for n, m in sarc_members(data)]
    for name, blob in blobs:
        for label, st in bnsh_variations(blob):
            counts['variations'] += 1
            if 'ms' not in st or 'ps' not in st:
                continue
            counts['mesh variations'] += 1
            mesh, pixel = hash_of.get(st['ms']), hash_of.get(st['ps'])
            task = hash_of.get(st['ts']) if 'ts' in st else None
            if not mesh or not pixel or ('ts' in st and not task):
                counts['unknown key'] += 1
                continue
            combos.add((task, mesh, pixel))
counts['distinct (task, mesh, pixel)'] = len(combos)
counts['with task'] = sum(1 for t, _, _ in combos if t)
print(dict(counts))
json.dump(sorted(combos, key=lambda c: (c[0] or '', c[1], c[2])), open(out_path, 'w'))
