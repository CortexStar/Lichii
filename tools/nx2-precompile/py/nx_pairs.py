"""nxnative's vertex/pixel pair tables, mapped to Lichii shader hashes.

usage: python -I nx_pairs.py <inventory.json> <profile dir> <out.json>
"""
import collections
import json
import os
import sys

inv_path, profile, out_path = sys.argv[1:4]
inv = json.load(open(inv_path))
by_skey = collections.defaultdict(set)
for r in inv:
    by_skey[r['skey']].add(r['code_hash'])
pairs = set()
for name in ('shader_pairs.tsv', 'pso_pairs.tsv'):
    for line in open(os.path.join(profile, name)):
        if line.startswith('#'):
            continue
        a, b = line.split()[:2]
        for ha in by_skey.get(a, ()):
            for hb in by_skey.get(b, ()):
                pairs.add((ha, hb))
json.dump(sorted(pairs), open(out_path, 'w'))
print('pairs', len(pairs))
