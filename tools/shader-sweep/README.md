# Offline shader compiler

`shader-sweep` runs captured instructions through Ampere lowering, Maxwell control-flow
analysis and translation, optimization passes, and SPIR-V emission. It does not launch
the emulator or a Vulkan device. Use `spirv-val` separately to validate its output.

## Build

The normal project build exposes the `shader-sweep` target with `ENABLE_DEBUG_TOOLS=ON`.
On Linux, a separate build needs only the common and shader-recompiler libraries:

```sh
sudo apt-get install cmake ninja-build g++ libboost-all-dev nlohmann-json3-dev \
  libssl-dev liblz4-dev libzstd-dev libcpp-httplib-dev libxbyak-dev python3 spirv-tools
cmake -S tools/shader-sweep -B build/shader-sweep -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/shader-sweep -j4
ctest --test-dir build/shader-sweep --output-on-failure
```

CMake 3.24 or newer is required. The standalone configuration downloads pinned fmt,
SPIRV-Headers, sirit, and ZBIC sources. It does not require system SPIRV-Headers.
The tests include synthetic CLI cases, parser bounds checks, and three small captured
regression programs. `spirv-val` is required when `BUILD_TESTING` is enabled.

## Inputs

Compile one extracted v46, v47, v48 or v49 serialized environment record:

```sh
shader-sweep --environment shader.env --cache-version 47 --output shader.spv
```

The record starts at its code-size field. Do not include a disk-cache header, pipeline
key, or neighboring records. Graphics records contain the program header and stage;
compute records contain the workgroup and shared-memory sizes. `--pipeline-key shader.key`
adds the captured 1056-byte graphics pipeline key. Compute keys are not accepted.

To rerun native lowering with captured specialization data:

```sh
shader-sweep capture.bin --environment shader.env --cache-version 47 \
  --pipeline-key shader.key --output shader.spv
```

Raw dumps default to instruction-only format. Use `--format native-header` only for
captures with a 128-byte native graphics header followed by 16-byte instructions.
The header supplies the exact stage. Instruction-only compilation needs `--stage`
or a captured environment. Graphics compilation also requires a header or environment;
the tool does not invent graphics input/output metadata.

The six explicit stage names are `vertex`, `tess-control`, `tess-eval`, `geometry`,
`fragment`, and `compute`. Conflicting explicit, header, and environment stages are errors.
Task, mesh, and vertex-A cache records report missing pipeline context.

For matching a raw capture against a cache, lower without choosing a graphics stage:

```sh
shader-sweep capture.bin --lower-only lowered.bin
```

This writes packed little-endian 64-bit Maxwell instructions, without a header,
scheduling words, or native address map. The reported stage is null when unknown.
Geometry-specific lowering requires `--stage geometry`. `--lower-only` does not run
control-flow translation or SPIR-V emission and is not a full compilation check.

## Offline context and profile

Missing constant-buffer values, texture descriptors, or storage-pointer classifications
are errors, not zero values. v46 records do not contain storage-pointer classifications.
Native trap guards require captured constant-buffer values and must verify before lowering.

For a compute allocation with an observed NVN2 c9 control record, `--control FILE`
reads the workgroup declaration from that record instead of using 1x1x1. This mode
rejects shaders that need an unknown local/shared-memory allocation, and still
requires real resource types, pointer classifications and constants whenever the
compiler reads them. It does not invent those values or create a driver pipeline.

The compiler reports assumptions in each JSON result. Without an environment, compute
uses a 1x1x1 workgroup and zero local/shared memory. Without a graphics pipeline key,
the runtime uses point input topology, isolines with equal tessellation spacing, float
interface types, and no alpha test or transform feedback. With a key, vertex input types,
fragment attachment types, topology, tessellation modes, alpha test, and depth/fragment
flags come from the capture. Transform feedback is rejected. Previous-stage varying
linkage is not reconstructed; all previous-stage varying slots are enabled.

The fixed Vulkan-style profile emits SPIR-V 1.4 with unified descriptor bindings,
one-byte minimum storage-buffer alignment, 32-lane subgroups, integer types, subgroup
votes/shuffles, geometry streams/passthrough, viewport/layer output, and eight clip
distances. Descriptor aliasing and native 8/16-bit storage access stay disabled to
exercise the compiler's portable fallbacks. Integer fragment attachments always use
typed outputs. Default emulator shader settings apply, with resolution scaling inactive.
This is an offline compiler exercise, not a driver-specific golden shader or proof of
correct in-game rendering.

## Results

Stdout contains one JSON object, except for `--help`. Stderr receives compiler warnings.
The object has `schema_version: 1`, `status`, `phase`, `stage`, `input_format`,
`source_bytes`, `spirv_words`, and `assumptions`. Lowering adds `lowered_words`.
Errors add `error.code` and `error.message`; lowering errors also include the native
instruction byte `error.offset`.

Exit status is 0 for successful emission, 1 for compilation/context/output failures,
and 2 for invalid arguments or input. Missing specialization data has phase `context`
and code `missing_context`. Compiler assertions routed through the common assertion
API become errors; a process-level crash in a dependency still needs to be detected
by the caller. Compilation failures do not overwrite the requested output.

```sh
spirv-val --target-env vulkan1.2 shader.spv
```

## Static archive inputs

`--capture-extent` accepts an allocated native code region and uses the runtime's
capture state machine to find its complete instruction extent. Padding is retained
in the extraction artifact, not treated as executable instructions. Missing
completion or invalid branch targets remain context failures.

The [all-library sweep report](../../docs/shader-sweep-all-games-20261104.md)
describes read-only inventory, NUP export, bounded archive extraction, preserved
control blobs, and static replay. Its cache associations are inferred, and its
synthetic-context validations are explicitly partial. Neither is a substitute for
capturing the original pipeline state.

## Replay extracted library shaders

Run inventory and extraction on the machine holding the library. Re-list it before
each source operation and skip files that disappeared. Use a fresh output directory
outside the library for every extraction. Examples use Bash variables.

```sh
python3 tools/nx2/inventory_games.py "$LIBRARY" --output "$WORK/inventory.json"
python3 tools/nx2/extract_static_shaders.py \
  --inventory "$WORK/inventory.json" --read-only-root "$LIBRARY" \
  --output "$WORK/static"
```

The Windows command is `py -3`; Python 3.14 supplies the zstd decoder there. Linux
uses the system `libzstd`. The bounded readers support NVN2 BNSH/BFSHA/SARC,
SHN/LZ4/SHDROUNC, and the observed DCX/BND4/CGSB layouts. They preserve complete
code and control allocations, SHA256s, container/member offsets and all origins.
Missing formats, unreadable sources and omitted inventory entries are errors.

The tools do not apply production fixes. To reproduce the coverage report's after column, build a
revision containing all three fixes listed in that report; the tooling branch keeps their code in
separate PRs.

Build the standalone compiler and exporter on Linux with clang and Ninja:

```sh
cmake -S tools/shader-sweep -B "$WORK/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DBUILD_TESTING=ON
cmake --build "$WORK/build" --parallel 8
ctest --test-dir "$WORK/build" --output-on-failure
```

The exporter can catalog a NUP without exporting its asset payloads. Explicit
selectors include the package index; inspect the catalog before choosing them.
The broad `--candidates` heuristic can include large non-shader archives, so it
is not a claim that every candidate is a shader.

```sh
"$WORK/build/nup-shader-export" --input "$GAME_NUP" \
  --read-only-root "$LIBRARY" --output "$WORK/catalog" --list
"$WORK/build/nup-shader-export" --input "$GAME_NUP" \
  --read-only-root "$LIBRARY" --output "$WORK/export" \
  --path '0:/Shader/example.bfsha'
```

`tools/shader-sweep/build_nup_windows.py` builds this exporter into scratch using
an already configured clang-cl build's headers and libraries read-only. Its six
compile jobs and all object, PDB and executable outputs stay under `--work-dir`.

Transfer only the extracted shader corpus to the compiler machine, then run:

```sh
python3 tools/nx2/sweep_static_shaders.py "$CORPUS/manifest.json" \
  --read-only-root "$CORPUS" --compiler "$WORK/build/shader-sweep" \
  --cache-directory "$CACHES" --output "$WORK/replay" --jobs 8
```

The replay requires `spirv-val`. Its output directory must be new and outside all
declared read-only roots, input manifests and cache directories. It checks artifact
hashes, retains distinct control blobs, and writes `run.json`, `results.jsonl`,
`results.json`, `archive-errors.json`, and `summary.md`. `run.json` records executable,
runner and manifest hashes. Any failed or partial coverage returns a nonzero exit.

`--capture-extent` uses the runtime's `ProgramCaptureState`; it does not search for
an arbitrary EXIT or invent a missing header. Compute input is bounded by the
observed c9 control record's declared code region before runtime extent discovery.
All 683 complete compute captures in this corpus fit that bound. The other eight
remain incomplete. Workgroup dimensions, resource descriptors and mesh launch
state are not guessed.

`--archive-info` and the production NUP preloader share the bounded C++ reader
for BNSH, AGL BAHS/SHARCB and uncompressed members of SARC archives. BAHS control
references may point backwards, and code allocations retain their C1 tails.
Task/mesh lowering is cached separately by stage; this does not reconstruct
their final pipeline linkage or launch state.

`shader-sweep --archive-info ARCHIVE` also reports BNSH stage groups and their
byte ranges. `shader-sweep --graphics-probe ARCHIVE NEW_OUTPUT_DIRECTORY` tests
exact VS/FS and task/mesh/fragment groups with shared bindings and the preceding
stage's real output interface. Mesh dimensions and payload come from c9 metadata;
triangles remain an explicit test contract. Its `report.json` states the contract and any missing
context. These modules are diagnostic artifacts, not ready-to-import runtime
pipelines; validate the emitted `.spv` files with `spirv-val`.

The optional `--float-textures` argument to `--graphics-probe` supplies an
explicit Color2D/RGBA8/non-integer metadata contract for direct C2 descriptors.
It does not supply raw handles. The report records all descriptor locations
used by this assumption; it is still a probe, not an importable runtime cache.

`pipeline-prewarm-test REPORT OUTPUT [LIMIT]` is a manual Vulkan hardware test
for reports from `--graphics-probe`. It requires `VK_LAYER_KHRONOS_validation`
and creates pipelines only: it issues no draw or dispatch. The output compares
first/repeated creation and a changed blend/depth state; the `.trace` companion
records progress. The first creation is not guaranteed to miss the driver's
disk cache, and the test does not measure cross-process cache persistence.

It writes `OUTPUT.cache.bin` when its application cache is enabled. An optional
fifth argument accepts that cache for a subsequent process, or `--no-app-cache`
to measure the driver's behavior without a Vulkan application cache. For example:

```sh
pipeline-prewarm-test report.json first.json 3
pipeline-prewarm-test report.json reused.json 3 first.json.cache.bin
pipeline-prewarm-test report.json uncached.json 3 --no-app-cache
```


### Graphics pipeline library probe

`pipeline-prewarm-test REPORT OUTPUT LIMIT --libraries` enables the Vulkan
`VK_EXT_graphics_pipeline_library` feature for a headless compilation experiment.
It builds the vertex-input (when present), pre-rasterization, fragment-shader and
fragment-output parts separately, then links them without link-time optimization.
Every trial recreates the parts through the same driver pipeline cache, so the
changed-state trials also measure cached part creation. The report distinguishes link timings from
`initial_with_libraries_ms` and `changed_with_libraries_ms`, which also include
part compilation. Validation layers remain required; no draw or dispatch occurs.
This probe does not start the emulator.

A serialized driver cache can be loaded in a separate library-mode process with:

```text
pipeline-prewarm-test report.json reloaded.json 14 saved.cache.bin --libraries
```


When depth-clip control and provoking-vertex support are available, the pipeline
probe adds isolated negative-one-to-one depth, last-provoking-vertex, and combined
HDR trials. `trial_labels` names every measurement in `trial_total_ms`. These are
controlled driver experiments; they do not reproduce all host-profile settings,
resource activity or concurrent compilation of a running game.
