# NUP shader and final pipeline preloading

## Behavior

When disk shader caching is enabled, a NUP launch prepares shaders during the
loading screen before starting guest execution. The preloader lowers supported
native programs, restores compatible captured pipelines, prepares eligible
compute variants, and warms graphics pipelines from linked archive stages and
supported material layouts. Compilation uses the existing worker pools.

This reduces first-use compiler work. It does not establish near-100% coverage
of every possible runtime specialization. Missing metadata follows the existing
runtime compilation path; required draws are not discarded to hide compilation.

## Cache and startup ordering

`Game.shaders.bin` is stored beside `Game.nup`. A checked index addresses compressed
native records by offset and optionally contains an opaque Vulkan driver-cache
record. Native records are independent of the GPU; driver records require the
matching NUP/compiler/device/settings profile. See [cache format and native
preloading](nup-shader-preload.md) for bounds, identity and publication rules.

The frontend calls `PrepareNupShaders` and `LoadDiskResources`, waits for their
workers, and only then starts the GPU/guest. A graphics-prewarm certificate is
written only after the attempts complete and the matching driver snapshot is
saved. It means those attempts finished, including deterministic deferrals; it
is not a certificate of complete gameplay coverage.

Switch 2 loads all compatible captured pipelines before gameplay and keeps them
resident. Compatible version-53 and version-54 environments are deduplicated and
rebuilt with the current compiler. Old-version driver binaries are not imported.
The normal application cache remains a compatibility fallback.

## Graphics pipeline libraries

On Switch 2, devices supporting both fast linking and independent interpolation
use `VK_EXT_graphics_pipeline_library` when the application driver cache is enabled.
The same splitter is used in the renderer and the headless validation tool. It
creates vertex-input, pre-rasterization, fragment-shader and fragment-output parts
as applicable, then links them without link-time optimization.

Every part uses the exact current state and the same complete descriptor layout.
The driver cache handles reuse; no approximate application-side lookup substitutes
one state for another. Library compilations are retained in the same driver blob
inside the NUP-side binary. Temporary library handles survive synchronous linking
and are then released. Unsupported devices, absent fragment stages and
rasterizer-discard cases retain monolithic creation. Library build errors retry
monolithic creation; device loss propagates.

Fast linking is not a guarantee of unchanged GPU throughput. Gameplay must verify
both rendering and frame times. Reference: [Khronos pipeline-library proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_graphics_pipeline_library.html).

## Material-derived vertex types

The bounded readers support the verified FRES 10.4.0 and FSHA 9.1.1 layouts. They
follow shape-to-material and shape-to-vertex-buffer indices, explicit material
assignments, and BFSHA attribute locations. Archive/model names and the embedded
BNSH offset connect each layout to the correct shader family.

Known types distinguish float/normalized, signed integer and unsigned integer.
Missing assignments, defaults and empty vertex records do not fabricate types.
Partial material records retain their known attributes and record missing names.

Each vertex program is translated to identify the generic inputs it consumes.
Candidates are deduplicated by those inputs, avoiding a full product of all
material layouts and shader states. Before emission, the final loads after legacy
attribute conversion must have matching declared and emitted types. An incomplete
candidate is deferred. Warmup pipelines never enter the runtime draw lookup.

The local NUP survey found 73 distinct material layouts in 8,128 model files.
Of these files, 779 have all-zero FVTX metadata; 105 more yielded known attributes
after partial-record support was added. These are metadata counts, not final
pipeline coverage percentages. The sword material establishes an unsigned integer
input at location 6; it does not establish framebuffer format or NDC convention.

## Compute and native-program constraints

BNSH and BAHS/SHARCB readers preserve actual C1 data and bounded code extents.
Compute C0 declarations identify pointer fields without inventing live addresses.
Static candidates require proven pointer origins, valid workgroup/local/shared
metadata, and device limits. Indirect loads, unresolved pointer branches and
ambiguous layouts are deferred.

The static storage route supports up to 64 buffers subject to device limits.
A 64-bit validity mask and full-access descriptor bounds guard address routing.
Runtime selection checks native code and every recorded constant dependency on
each dispatch; indistinguishable candidates fall back to runtime compilation.

The upstream null-pointer rebuild path is preserved: when a pointer omitted at
translation becomes mapped, a replacement includes already known pointers. A
failed rebuild keeps the previous pipeline, whose lifetime covers pending GPU use.

## Saving and shutdown

Driver-cache publication uses a checked snapshot and atomic replacement. A failed,
cancelled or stale write preserves the previous binary. Identical verified driver
records are not rewritten. Shutdown grants an active cache write time to finish,
bounded by 120 seconds without progress, rather than immediately terminating its
thread. This also covers GPU teardown leading to the snapshot. Runtime testing
must confirm final saves complete in the affected game session.

## Evidence and validation

The reported sword/foliage session used an older build. Its slowest recorded
pipeline was `6f4048d575f1d190`, with vertex `343b13756efa326a` and fragment
`5c04230f908006b2`; worker emission plus driver creation took 596.2 ms. The session
also ended in forced termination without a final driver-save confirmation.
Neither observation proves that every reported freeze was compiler work.

Validated on Windows with clang-cl Release and 12 build jobs:

- Native archive, cache round-trip, profile mismatch, corruption, cancellation,
  stale-writer and source-preservation checks.
- Five NUP/cache/material integration tests, including cross-linked signed and
  unsigned materials and rejection of an unrelated shader model.
- Forty shader CLI tests and 37 Python archive/container tests in the preceding
  integration; subsequent material changes have targeted parser and cache checks.
- Consumed-input completeness and emitted-type equality checks, including rejection
  of an integer material input emitted as float.
- Eight synthetic SPIR-V modules validated for Vulkan 1.2 and 58 synthetic buffer
  dispatch cases on an RTX 4070 Ti, including bindings 32 and 63.
- Fourteen NUP foliage groups compiled/linked through the shared library splitter
  with dynamic states and zero validation errors. Recreating cached parts for
  changed state took 0.09315 ms median; this is pipeline creation, not frame time.
- A separate process restored a 3,353,260-byte driver cache. First creation took
  4.9175 and 18.1598 ms for two groups; changing the first attachment to
  B10G11R11_UFLOAT_PACK32 took 0.0505 and 0.0362 ms, with no validation errors.

No emulator or game was started for these checks. The new executable still needs
a user-run sword/foliage test, rendering inspection, frame-time comparison and
confirmation that new runtime compilations are reused after reopening the game.

## Reproducing headless checks

Build the explicit test targets with the normal project configuration. The tools
operate on supplied fixtures/archives and do not launch guest execution:

```text
static-buffer-bounds-test OUTPUT_DIRECTORY
static-buffer-bounds-gpu-test OUTPUT_DIRECTORY
archive-vertex-layout-test [DECOMPRESSED_BFRES_OR_BFSHA ...]
nup-shader-precompile GAME.nup --vertex-info
pipeline-prewarm-test REPORT.json OUTPUT.json 14 --libraries
pipeline-prewarm-test REPORT.json RELOADED.json 14 SAVED.cache.bin --libraries
```

`--vertex-info` reports metadata candidates rather than successful Vulkan builds.
The pipeline probe requires validation layers and reports linking separately from
part creation, including totals. A failed build is not counted as a successful
prewarm.

## Upstream basis and open checks

The integration includes compatible-pipeline retention from Eglantine
[PR #696](https://github.com/Djihads80/Eglantine/pull/696) / [#708](https://github.com/Djihads80/Eglantine/pull/708),
the 64-buffer/null-pointer work imported by Lichii #551, and unused-prebuilt
retention from #550. The native wait policy remains Wait; optional bounded/skip
modes are not enabled by this change.

Still unproven:

- Near-100% final pipeline coverage on the first NUP-only launch.
- Elimination of the reported sword/foliage freeze in the current executable.
- Rendering and steady-state GPU throughput with pipeline libraries in this game.
- Complete shutdown-save and subsequent-session reuse under the user's workload.
- Recovery of resource/state values absent from the supported archive metadata.


## Re-entry cache-key investigation

A subsequent game session completed preload and clean shutdown saves. It recorded
230 graphics builds with 230 distinct pipeline hashes, so the log did not show
repeated compilation of an identical key. However, eight keys for one confirmed
task/mesh/fragment group differed only in dynamic vertex-input types. Mesh pipelines
do not consume that vertex-input state. Their seven redundant builds consumed
169.1 ms in total on the GPU thread; this is accumulated work, not one frame time.

The mesh key now clears dynamic vertex-input types consistently for runtime
lookup, old captured cache records, and NUP prewarm. The original dirty-state
shadow is restored after lookup so a subsequent vertex draw retains its input
types. Static input/divisor state is unchanged. In a replay of the saved keys,
1583 entries reduce to 1368; 230 logged runtime keys reduce to 207. A direct C++
regression also reduces the nine extracted sample keys to two distinct keys.

The public NVN/NVN2 reimplementation similarly normalizes irrelevant state in its
[pipeline keys](https://github.com/seedds/nxnative/blob/bd979ced3ae6bad60bf86429a71d172297d93471/dm/gfx/nvn/nvn_pso_skey.h)
and separates reusable pipeline bases from specialization in its
[Metal backend](https://github.com/seedds/nxnative/blob/bd979ced3ae6bad60bf86429a71d172297d93471/dm/gfx/nvn/nvn_mtl.m).
This is a reference implementation, not direct evidence of Nintendo's proprietary
driver internals. Vulkan also specifies that mesh pipelines omit vertex-input
state. The change uses that verified boundary rather than copying Metal behavior.

The NUP preload also omits the second blend/depth baseline when both states are
dynamic. Worker timing now reports SPIR-V emission separately from driver pipeline
construction. The largest observed 908.5 ms build used another shader group with
no duplicate key in the saved corpus, so this patch does not explain or claim to
eliminate that stall. Gameplay validation remains required.
