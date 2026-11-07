#!/usr/bin/env bash
# Builds a Switch 2 title's precompiled pipeline file (precompiled.fork-v<N>.nx2prep) from nxnative's
# build output for the title and the Lichii disk caches recorded while playing it.
#
# usage: run_pipeline.sh CACHE.bin [CACHE.bin...]
#
# Environment:
#   NX2_BUILD    nxnative's build folder of the title (<title>_build: shader/pkg, extract/v*/RomFS.img)
#   NX2_PROFILE  nxnative's profile folder of the title (dm/games/<title>: shader_map.tsv, shader_pairs.tsv,
#                pso_pairs.tsv)
#   NX2_OUT      output folder (default ./nx2-precompile-out)
#   NX2_TOOL     nx2-precompile executable (default: nx2-precompile on PATH)
#   NX2_K        predicted render states per vertex/pixel pair (default 4)
#   NX2_CACHE_VERSION  pipeline cache version, the file name's v<N> (default: the one NX2_TOOL was
#                built with; it must match the Lichii build that reads the file)
#   PYTHON       Python 3.11+ with the zstandard module (default python)
set -euo pipefail

: "${NX2_BUILD:?set NX2_BUILD to the nxnative build folder of the title}"
: "${NX2_PROFILE:?set NX2_PROFILE to the nxnative profile folder of the title}"
[ $# -ge 1 ] || { echo "usage: $0 CACHE.bin [CACHE.bin...]" >&2; exit 2; }
OUT=${NX2_OUT:-$PWD/nx2-precompile-out}
TOOL=${NX2_TOOL:-nx2-precompile}
K=${NX2_K:-4}
VERSION=${NX2_CACHE_VERSION:-$("$TOOL" cache-version)}
PREP=$OUT/precompiled.fork-v$VERSION.nx2prep
PYTHON=${PYTHON:-python}
PY=$(cd "$(dirname "$0")" && pwd)/py
PKG=$NX2_BUILD/shader/pkg
ROMFS=$(ls "$NX2_BUILD"/extract/v*/RomFS.img | sort -V | tail -1)
PRIORS=$OUT/priors.json
TRAIN=()
for cache in "$@"; do TRAIN+=(--train "$cache"); done
mkdir -p "$OUT"

echo "== 1. storage-pointer priors from the recorded caches"
"$PYTHON" -I "$PY/learn_priors.py" "$PRIORS" "$@"

echo "== 2. oracle accuracy against each recorded cache"
for cache in "$@"; do
    "$TOOL" validate --pkg "$PKG" --cache "$cache" --out "$OUT/validate_$(basename "$cache" .bin)" \
        --priors "$PRIORS" | grep -v differs:
done

echo "== 3. every package program: lowered, hashed and given oracle facts"
"$TOOL" scan --pkg "$PKG" --out "$OUT/scan" --priors "$PRIORS"

echo "== 4. inventory, vertex/pixel pairs, mesh variations"
"$PYTHON" -I "$PY/inventory.py" "$PKG" "$OUT/scan.tsv" "$NX2_PROFILE/shader_map.tsv" "$OUT/inventory.json"
"$PYTHON" -I "$PY/nx_pairs.py" "$OUT/inventory.json" "$NX2_PROFILE" "$OUT/nx_pairs.json"
"$PYTHON" -I "$PY/mesh_variations.py" "$ROMFS" "$OUT/inventory.json" "$OUT/mesh_combos.json"
"$PYTHON" -I "$PY/mesh_draws.py" "$PKG" "$OUT/inventory.json" "$OUT/mesh_combos.json" "$OUT/mesh_draws.tsv"
"$TOOL" mesh-envs --pkg "$PKG" --cache "$OUT/mesh_draws.tsv" --out "$OUT/mesh" --priors "$PRIORS"

if [ $# -ge 2 ]; then
    echo "== 5. held-out render-state accuracy (learn from the first cache, test the second's new pipelines)"
    "$PYTHON" -I "$PY/state_eval2.py" "$OUT/inventory.json" "$OUT/nx_pairs.json" "$1" "$2"
fi

echo "== 6. precompiled file"
"$PYTHON" -I "$PY/build_precompiled.py" --inventory "$OUT/inventory.json" --envs "$OUT/scan.envs" \
    --pairs "$OUT/nx_pairs.json" "${TRAIN[@]}" --k "$K" \
    --mesh-combos "$OUT/mesh_combos.json" --mesh-envs "$OUT/mesh.mesh" --mesh-k 2 \
    --cache-version "$VERSION" --out "$PREP" --report "$OUT/precompiled_report.json"

echo "== 7. replay check"
"$TOOL" check --file "$PREP"

echo "== 8. SPIR-V for every environment, one at a time (a compiler abort is listed, not fatal)"
"$PYTHON" -I "$PY/emit_check.py" "$TOOL" "$PREP" "$OUT/emit_crashes.json"
echo "done: copy $PREP next to the title's vulkan.fork-v$VERSION.bin"
