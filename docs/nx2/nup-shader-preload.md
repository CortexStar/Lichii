# NUP shader preloading

The frontend prepares NUP-owned native shader code during the existing loading
screen, before starting emulation. This uses the disk-shader-cache setting.
The CLI offers the same preparation without booting a game:

```sh
eden-cli --precompile-nup "Game.nup"
```

`nup-shader-precompile`, built with the debug tools, calls the same implementation.
It exists for tests and offline inspection; normal users do not need to run it.

## What the cache contains

`Game.shaders.bin` is stored beside `Game.nup`. It contains native programs lowered
to the compiler's Maxwell representation, with their original address maps and
texture-buffer requirements. Records are individually Zstandard-compressed and
decoded when first needed. The file's header points to a checksummed index at the
end of the file; each index entry names a record's offset, size and checksum.
Runtime lookup reads only that record instead of loading the entire cache.
After a complete graphics prewarm, the same indexed file can also contain a
compressed Vulkan driver-cache record, keyed by the NUP/compiler/device/settings
profile. This record is opaque and device-specific; the native records remain
independently readable. Captured pipeline environments still use the existing
application cache.

The cache identity includes the NUP header, size, modification time, format version,
and a source fingerprint covering the compiler and preload implementation. A
SHA-256 checksum covers the serialized contents. A stale or corrupt cache is
rebuilt. Publication uses a unique temporary file beside the destination; a
cancelled preparation does not replace an existing cache. Runtime lookup checks
the stage, original native bytes, and geometry lowering mode before reuse. Normal
trap-guard verification and diagnostic shader rejection still apply.

Compilation uses the project's `ThreadWorker` pool. Queueing is bounded by the
worker count; the producer works on the current shader when the pool is full.
Workers compile and compress independently, then briefly lock the output stream
to append a complete record and register its offset. The index is written after
all workers finish. Cancellation joins the workers before discarding the temporary
file, and does not publish a partial cache.

Updating the driver record copies the native record region unchanged to a
unique temporary file, writes the replacement driver payload and index, then
atomically publishes it. Driver data has its own checksum and size bounds.
An incompatible or corrupt driver record is ignored without discarding native
shader records. Failed/cancelled updates preserve the previous binary, and a
stale writer cannot replace a newer index. The normal application driver cache
remains a compatibility fallback.

An identical, already verified driver record is checked through its compressed
checksum and left in place, avoiding a rewrite of the native region on every
launch. A driver record is bounded to 8 GiB; allocation or I/O failures keep
the previous cache and use the application fallback. Tests cover reopening,
replacement, corruption, profile mismatch, cancellation and stale writers.

## Scope and remaining runtime work

The reader handles embedded BNSH and AGL BAHS/SHARCB programs, including SARC
members and outer Zstandard frames with dictionaries. It merges the base RomFS with the
selected update and inspects add-on RomFS archives. Unsupported archives and
programs remain in the cache's `pending` diagnostics.

Task and mesh instructions are also lowered and indexed by their own stages.
Their final pipelines still require the normal mesh linkage and launch metadata.
The BAHS reader preserves the complete bounded code allocation, including C1;
shared control records use signed relative offsets. Nested compressed SARC
members are not decoded by this reader.

Preloading cannot derive texture descriptors, constant-buffer values, storage
pointer classifications, workgroup dimensions, mesh launch state, or complete
graphics pipeline state from native instructions alone. Those remain runtime
inputs. Trap-dependent lowering and address-dependent instructions are deferred
when their required context is unavailable. Lowering success therefore does not
mean SPIR-V or driver-pipeline compilation has finished, and this feature alone
does not establish an FPS improvement or eliminate first-use stutter.

## Validation status

The bounded archive reader has executable regressions for stage identification,
invalid extents, embedded programs, signed shared controls, C1 preservation,
SARC member bounds and cancellation. The native preload CLI has cold/warm-cache,
source-change, corrupt
cache and source-preservation tests. Twenty repetitions of the native cache test
passed, including concurrent offset lookup. The graphical frontend and CLI built
successfully in Release mode on Windows. The user subsequently reported a
substantial improvement with remaining stutters. The session log confirms NUP
compute cache hits and additional runtime graphics pipeline builds; see the
evidence and validation section in `final-pipeline-preload.md`. This is not a controlled
performance benchmark or complete rendering validation.

After adding BAHS/SARC and task/mesh lowering, the direct Zelda NUP run produced
47,823 lowered stage/code entries and 59 pending diagnostics, compared with
46,150 lowered entries before this expansion. This is coverage of the supported
archive inventory, not a count of final pipelines or all runtime variants.
The C++ reader and the independent Python extractor agree on all 4,874 records
in the inspected AGL application SARC: 1,799 vertex, 2,118 fragment, 470 compute,
168 task and 319 mesh records.

For static compiler coverage, `shader-sweep --archive-info FILE` inspects the same
BNSH reader. The existing `sweep_static_shaders.py` distinguishes native lowering,
missing runtime context, and SPIR-V validation with synthetic pipeline defaults.
Synthetic defaults are not proof that an emitted shader matches a game's pipeline.

## Static metadata investigation

The Zelda NUP's recovered corpus contains 60,332 distinct stage/code identities.
The bounded metadata reader found nonempty BNSH reflection for 33,760 of them,
and c9 workgroup declarations for 4,141 of 4,601 compute identities. Reflection
includes resource names, but does not establish the current values or bindings
of those resources. These are metadata coverage counts, not additional compiled
pipelines.

`inspect_bnsh_reflection` retains resource dictionaries and raw scalar fields;
`declared_compute_workgroup` reads the observed NVN2 c9 declaration.
`inspect_bfsha_models` reads the observed version-9 model layout and preserves
absent uniform defaults as `None`, distinct from actual zero-filled defaults.
For example, the inspected Foliage archive has default data for 14 of 162 uniform
blocks; SceneMaterial has it for one of four. Values absent from these blocks
must not be fabricated. They may be supplied by other assets or by the game.

Layout references are the pinned ShaderLibrary
[BNSH reader](https://github.com/KillzXGaming/ShaderLibrary/blob/790dd2e501926e536a9193907d1256d446104162/ShaderLibrary/Switch/BnshLoader.cs)
and [BFSHA reader](https://github.com/KillzXGaming/ShaderLibrary/blob/790dd2e501926e536a9193907d1256d446104162/ShaderLibrary/Switch/BfshaLoader.cs).
The C++ c9 reader now supplies eligible compute environments to the Vulkan
preloader with per-dispatch compatibility guards. The Python reflection readers
remain investigative tooling; reflection alone does not supply complete graphics
pipeline state. See [final-pipeline-preload.md](final-pipeline-preload.md) for the
current admission counts and limits.
