"""Joins nx2-precompile's scan with nxnative's shader map: one row per package program.

usage: python -I inventory.py <pkg dir> <scan.tsv> <shader_map.tsv> <out.json>
"""
import hashlib, json, os, struct, sys


def skey(ctl, code):
    if len(ctl) >= 0x48 and ctl[:4] == bytes.fromhex('34127698') and ctl[4:8] == b'\xc9\0\0\0':
        c = ctl[:0x48]
        c = c[:0x30] + bytes(4) + c[0x34:]
    else:
        n = struct.unpack_from('<I', ctl, 0x20)[0]
        c = ctl[:n if 0x760 <= n < 0x880 else 0x880]
    return hashlib.sha256(c + code).hexdigest()[:16]


pkg, scan_path, map_path, out_path = sys.argv[1:5]
scan = {}
for line in open(scan_path, encoding='utf-8').read().splitlines()[1:]:
    f = line.split('\t')
    scan[(f[0], int(f[1]))] = f
source = {}
for line in open(map_path, encoding='utf-8'):
    if line.startswith('#') or line.startswith('package\t'):
        continue
    f = line.rstrip('\n').split('\t')
    source.setdefault(f[3], f[4])
rows = []
for name in sorted(os.listdir(pkg)):
    if name.startswith('._') or not name.endswith('.bin'):
        continue
    d = open(os.path.join(pkg, name), 'rb').read()
    o = idx = 0
    while o < len(d):
        cl, kl = struct.unpack_from('<2I', d, o + 0x28)
        code = d[o + 0x38:o + 0x38 + kl]
        ctl = d[o + 0x38 + kl:o + 0x38 + kl + cl]
        k = skey(ctl, code)
        s = scan[(name, idx)]
        src = source.get(k, '')
        rows.append(dict(file=name, index=idx, ordinal=len(rows), stage=s[2], status=s[3],
                         code_hash=s[4], skey=k, source=src,
                         fine=src.split('#var')[0], coarse=src.split('#')[0]))
        o += 0x38 + kl + cl
        idx += 1
json.dump(rows, open(out_path, 'w'))
print('programs', len(rows), 'with source', sum(1 for r in rows if r['source']))
