"""Writes a game's precompiled pipeline file (NX2PREP1) for Lichii's PreparePrecompiled.

Records, all with canonical keys (statefields.canonical, as FixedPipelineState::CanonicalizeNative):
  * every pipeline recorded in the training caches, with its recorded environments;
  * for every vertex/pixel pair (nxnative's tables and the recorded ones), the K most likely
    render states the state model predicts, with the oracle environments of nx2-precompile;
  * every compute program, with its oracle environment (a compute key holds no render state).

usage: python -I build_precompiled.py --inventory inv.json --envs scan.envs --pairs nx_pairs.json
           --train a.bin [--train b.bin ...] --k 3 --cache-version N --out precompiled.fork-vN.nx2prep
"""
import argparse
import collections
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lichii_cache as lc  # noqa: E402
import state_model as sm  # noqa: E402
import statefields as sf  # noqa: E402

GRAPHICS_KEY = 1056
STATE_TAIL = GRAPHICS_KEY - 48 - sf.STATE_LEN
XFB_BIT = 1 << 6

ap = argparse.ArgumentParser()
ap.add_argument('--inventory', required=True)
ap.add_argument('--envs', required=True)
ap.add_argument('--pairs', required=True)
ap.add_argument('--train', action='append', required=True)
ap.add_argument('--k', type=int, default=3)
ap.add_argument('--out', required=True)
ap.add_argument('--report')
ap.add_argument('--mesh-combos', help='(task, mesh, pixel) combinations, mesh_variations.py')
ap.add_argument('--mesh-envs', help='NXPMESH1 file, nx2-precompile mesh-envs')
ap.add_argument('--mesh-k', type=int, default=2)
ap.add_argument('--cache-version', type=int, required=True,
                help="the Lichii build's VulkanPipelineCacheVersion (the file name's v<N>)")
args = ap.parse_args()

inv = json.load(open(args.inventory))
by_hash = collections.defaultdict(list)
for r in inv:
    by_hash[r['code_hash']].append(r)

# Oracle environments from nx2-precompile scan (one per lowered code).
oracle = {}
data = open(args.envs, 'rb').read()
assert data[:8] == b'NXPENV01'
o = 8
while o < len(data):
    ordinal, code_hash, size = struct.unpack_from('<IQI', data, o)
    o += 16
    h = '%016x' % code_hash
    oracle.setdefault(h, data[o:o + size])
    o += size
print('oracle environments', len(oracle))


def env_tail_compute(blob):
    """(workgroup, shared) from the end of a serialized compute environment."""
    wx, wy, wz, shared = struct.unpack_from('<4I', blob, len(blob) - 16)
    return (wx, wy, wz), shared


class Writer:
    def __init__(self):
        self.envs = []
        self.env_ids = {}
        self.records = []
        self.keys = set()

    def env(self, blob):
        digest = hashlib.sha256(blob).digest()
        if digest not in self.env_ids:
            self.env_ids[digest] = len(self.envs)
            self.envs.append(blob)
        return self.env_ids[digest]

    def graphics(self, blobs, key):
        assert len(key) == GRAPHICS_KEY
        h = lc.graphics_key_hash(key)
        if h in self.keys:
            return False
        self.keys.add(h)
        self.records.append((0, [self.env(b) for b in blobs], key))
        return True

    def compute(self, blob, key):
        assert len(key) == 24
        if key in self.keys:
            return False
        self.keys.add(key)
        self.records.append((1, [self.env(blob)], key))
        return True

    def write(self, path):
        header = struct.pack('<8sIIII', b'NX2PREP1', args.cache_version, len(self.envs), len(self.records), 0)
        index_size = 16 * len(self.envs)
        body = bytearray()
        for kind, ids, key in self.records:
            body += struct.pack('<BBH', kind, len(ids), 0) + struct.pack('<%dI' % len(ids), *ids) + key
        offset = len(header) + index_size + len(body)
        index = bytearray()
        for blob in self.envs:
            index += struct.pack('<QQ', offset, len(blob))
            offset += len(blob)
        tmp = path + '.partial'
        with open(tmp, 'wb') as f:
            f.write(header)
            f.write(index)
            f.write(body)
            for blob in self.envs:
                f.write(blob)
        os.replace(tmp, path)


def unique_hashes(vs, fs, mesh_slots=None):
    hs = [0] * 6
    hs[1] = vs
    hs[5] = fs
    return struct.pack('<6Q', *hs)


def canonical_key(rec):
    """A recorded graphics key, canonical, with the bytes past the hashed state cleared."""
    key = rec.key
    vertex = next((e for e in rec.envs if e.stage == 0), None)
    mesh = any(e.stage in (7, 8) for e in rec.envs)
    mask = 0 if mesh else (sf.attribute_read_mask(vertex.sph) if vertex else 0xFFFFFFFF)
    state = sf.canonical(key[48:48 + sf.STATE_LEN], mask)
    return key[:48] + state + bytes(STATE_TAIL)


w = Writer()
model = sm.Model()
mesh_model = sm.Model()
recorded_pairs = set()
counts = collections.Counter()


def shader_info(env, vertex):
    h = '%016x' % lc.native_shader_hash(env.code)
    rows = by_hash.get(h, [{}])
    d = dict(hash=h, fine=rows[0].get('fine', '?'), coarse=rows[0].get('coarse', '?'))
    if vertex:
        d['read_mask'] = sf.attribute_read_mask(env.sph)
    else:
        d['omap'] = sm.omap(env.sph)
    return d


for path in args.train:
    _, recs = lc.parse_cache(open(path, 'rb').read(), mesh_fields=True)
    for rec in recs:
        blobs = [e.serialize(True) for e in rec.envs]
        if rec.is_compute:
            counts['recorded compute'] += w.compute(blobs[0], rec.key)
            continue
        if any(e.stage in (7, 8) for e in rec.envs) and any(
                e.stage == 8 and (e.mesh_registers[0] & 0xF) != 4 for e in rec.envs):
            counts['recorded mesh skipped (topology)'] += 1
            continue
        key = canonical_key(rec)
        counts['recorded graphics'] += w.graphics(blobs, key)
        if rec.envs[0].stage == 0 and rec.envs[-1].stage == 4 and len(rec.envs) == 2:
            vs, fs = shader_info(rec.envs[0], True), shader_info(rec.envs[-1], False)
            model.add(vs, fs, key[48:48 + sf.STATE_LEN])
            recorded_pairs.add((vs['hash'], fs['hash']))
        elif any(e.stage == 8 for e in rec.envs) and rec.envs[-1].stage == 4:
            mesh_env = next(e for e in rec.envs if e.stage == 8)
            ms, fs = shader_info(mesh_env, True), shader_info(rec.envs[-1], False)
            ms['read_mask'] = 0
            mesh_model.add(ms, fs, key[48:48 + sf.STATE_LEN])

pairs = set(tuple(p) for p in json.load(open(args.pairs))) | recorded_pairs
print('pairs', len(pairs))


def oracle_info(h, vertex):
    rows = by_hash.get(h, [{}])
    blob = oracle[h]
    # The SPH sits right after the code in a serialized graphics environment; the code starts
    # with it too (lowered code begins with the header).
    sph = blob[76:76 + 0x50]
    d = dict(hash=h, fine=rows[0].get('fine', '?'), coarse=rows[0].get('coarse', '?'))
    if vertex:
        d['read_mask'] = sf.attribute_read_mask(sph)
    else:
        d['omap'] = sm.omap(sph)
    return d


for vh, fh in sorted(pairs):
    if vh not in oracle or fh not in oracle:
        counts['pair without oracle environment'] += 1
        continue
    vs, fs = oracle_info(vh, True), oracle_info(fh, False)
    added = 0
    for state in model.candidates(vs, fs, args.k + 2):
        if struct.unpack_from('<I', state, 0)[0] & XFB_BIT:
            continue
        key = unique_hashes(int(vh, 16), int(fh, 16)) + state + bytes(STATE_TAIL)
        if w.graphics([oracle[vh], oracle[fh]], key):
            added += 1
            counts['predicted graphics'] += 1
        if added >= args.k:
            break

if args.mesh_combos and args.mesh_envs:
    # Mesh draws: the mesh model knows the states mesh pipelines were drawn with; the vertex model's
    # target tables cover pixel shaders no mesh draw used yet.
    for table in ('F',):
        for key_, counter in getattr(model, table).items():
            if key_ not in getattr(mesh_model, table):
                getattr(mesh_model, table)[key_] = counter
    data = open(args.mesh_envs, 'rb').read()
    assert data[:8] == b'NXPMESH1'
    o = 8
    draws = collections.defaultdict(list)   # (task, mesh) -> [(state hash, task env, mesh env)]
    while o < len(data):
        line, state_hash, task, mesh = struct.unpack_from('<IQQQ', data, o)
        o += 28
        (tsize,) = struct.unpack_from('<I', data, o)
        tblob = data[o + 4:o + 4 + tsize]
        o += 4 + tsize
        (msize,) = struct.unpack_from('<I', data, o)
        mblob = data[o + 4:o + 4 + msize]
        o += 4 + msize
        draws[(task, mesh)].append((state_hash, tblob, mblob))
    for task_h, mesh_h, pixel_h in json.load(open(args.mesh_combos)):
        task, mesh = int(task_h, 16) if task_h else 0, int(mesh_h, 16)
        if pixel_h not in oracle or (task, mesh) not in draws:
            counts['mesh draw without environments'] += 1
            continue
        rows = by_hash.get(mesh_h, [{}])
        ms = dict(hash=mesh_h, fine=rows[0].get('fine', '?'), coarse=rows[0].get('coarse', '?'),
                  read_mask=0)
        fs = oracle_info(pixel_h, False)
        states = mesh_model.candidates(ms, fs, args.mesh_k)
        for state_hash, tblob, mblob in draws[(task, mesh)]:
            hs = [0] * 6
            hs[5] = int(pixel_h, 16)
            if task:
                hs[1], hs[3] = task ^ state_hash, mesh ^ state_hash
                blobs = [tblob, mblob, oracle[pixel_h]]
            else:
                hs[1] = mesh ^ state_hash
                blobs = [mblob, oracle[pixel_h]]
            for state in states:
                key = struct.pack('<6Q', *hs) + sf.canonical(state, 0) + bytes(STATE_TAIL)
                counts['predicted mesh'] += w.graphics(blobs, key)

for h, blob in sorted(oracle.items()):
    rows = by_hash.get(h, [])
    if not rows or rows[0]['stage'] != 'Compute':
        continue
    workgroup, shared = env_tail_compute(blob)
    key = struct.pack('<QI3I', int(h, 16), shared, *workgroup)
    counts['oracle compute'] += w.compute(blob, key)

w.write(args.out)
size = os.path.getsize(args.out)
summary = dict(counts, environments=len(w.envs), records=len(w.records), bytes=size)
print(json.dumps(summary, indent=1))
if args.report:
    json.dump(summary, open(args.report, 'w'), indent=1)
