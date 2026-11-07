"""(task, mesh, lines) draws for nx2-precompile mesh-envs, from the mesh variations.

With a task program, INIT's `lines` is the task payload in 128-byte units (control +0x5a): 256 -> 2,
1152 -> 9, 3200 -> 25 in every recorded draw. Without one, INIT keeps what the last task draw set,
so each of the recorded values is prepared.

usage: python -I mesh_draws.py <pkg dir> <inventory.json> <mesh_combos.json> <out.tsv>
"""
import json
import os
import struct
import sys

pkg, inv_path, combos_path, out_path = sys.argv[1:5]
inv = json.load(open(inv_path))
where = {}
for r in inv:
    if r['stage'] == 'Mesh':
        where.setdefault(r['code_hash'], (r['file'], r['index']))
controls = {}
for name in sorted(os.listdir(pkg)):
    if not name.startswith('ms_') or name.startswith('._'):
        continue
    d = open(os.path.join(pkg, name), 'rb').read()
    o = idx = 0
    while o < len(d):
        cl, kl = struct.unpack_from('<2I', d, o + 0x28)
        controls[(name, idx)] = d[o + 0x38 + kl:o + 0x38 + kl + cl]
        o += 0x38 + kl + cl
        idx += 1
draws = set()
for task, mesh, pixel in json.load(open(combos_path)):
    ctl = controls[where[mesh]]
    if task:
        payload = struct.unpack_from('<H', ctl, 0x5A)[0]
        draws.add((task, mesh, max(1, payload // 128)))
    else:
        for lines in (25, 2, 9):
            draws.add(('-', mesh, lines))
with open(out_path, 'w') as f:
    for task, mesh, lines in sorted(draws):
        f.write('%s %s %d\n' % (task, mesh, lines))
print('mesh draws', len(draws))
