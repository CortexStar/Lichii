# nx2-precompile

Builds Vulkan pipelines for a Switch 2 game's shaders before they are first drawn, so a new area does
not stall while the driver compiles them.

## How it works

1. `nx2-precompile scan` reads every native program from nxnative's shader packages (`BTCD` records),
   captures and lowers it as `GenericEnvironment::LowerNativeCode` does, and computes the shader hash
   the emulator will compute at runtime. It then translates each program against a static fact oracle
   and writes the facts the translator asked for, in `GenericEnvironment::Serialize` layout.
2. The Python tools in `py/` predict the render states each shader combination is drawn with, from
   the pipelines the recorded caches hold, and write the game's precompiled file (`NX2PREP1`).
   Vertex/pixel pairs come from nxnative's pair tables; mesh, task and pixel combinations from the
   variation tables of the game's BNSH archives (`mesh_variations.py`), with their mesh state
   (`SET_MESH_SHADER_A/B`, `SET_MESH_INIT_SHADER` and the pass-through geometry header) derived from
   the mesh program's control section by `nx2-precompile mesh-envs`.
3. At boot, after the disk cache is loaded, `PipelineCache::PreparePrecompiled` builds every pipeline
   of that file once and drops it. The driver keeps the compiled code in its own cache. A draw still
   translates the guest program with its real facts and builds its own pipeline; when that pipeline is
   one the file predicted, the driver finds it compiled.

The oracle never decides what a draw runs: a wrong guess only means the driver compiles that pipeline
when it is first drawn, as it does without this tool.

Native keys are canonical (`FixedPipelineState::CanonicalizeNative`): attribute types of inputs the
vertex program does not read, blend factors of attachments without blending, attachments past the
last colour target, and the depth bounds are cleared. None of them changes the pipeline, and the
precompiled keys can only match draws if both sides clear them. The attribute mask comes from the
vertex program's header; a program found to load an attribute its header does not list is logged,
its pipeline is dropped once, and its keys keep every attribute type from then on.

The preparation step needs a driver that keeps its own pipeline cache (NVIDIA and AMD's proprietary
drivers, as `DriverCachesPipelines` lists); elsewhere it runs only with `EDEN_NX2_PRECOMPILED=1`.
Records carry the driver of the machine that recorded them; they are prepared with this machine's,
as draws key them, so a driver update only means preparing again.

The driver's cache must be large enough to hold the whole file. **On NVIDIA, set Shader Cache Size to
Unlimited** (NVIDIA Control Panel, Manage 3D settings). With "Driver default" the driver kept about
1 GiB for the emulator: of the 132,312 pipelines prepared for Ocarina of Time it stored the first
~33,000 and silently dropped the rest, so a new area compiled almost everything again. Unlimited
held all of them (about 2.6 GB on an RTX 3080). The preparation step cannot detect this; a session's
log shows it as prepared pipelines that still take hundreds of milliseconds to build.

## Commands

```
nx2-precompile scan     --pkg DIR --out PREFIX [--priors FILE] [--threads N]
nx2-precompile validate --pkg DIR --cache CACHE.bin --out PREFIX [--priors FILE]
nx2-precompile mesh-envs --pkg DIR --cache DRAWS.tsv --out PREFIX [--priors FILE]
nx2-precompile check    --file precompiled.fork-v<N>.nx2prep
nx2-precompile cache-version
```

`run_pipeline.sh CACHE.bin...` runs the whole chain (about 8 minutes) from nxnative's build output of
the title (`NX2_BUILD`, its shader packages and RomFS image) and profile (`NX2_PROFILE`, its shader
map and pair tables) and the disk caches recorded while playing it; see the script's header for the
other variables. Python 3.11 or newer with `zstandard` reads the BNSH archives, which use the title's
zstd dictionaries. `analyze_session.sh` sorts the pipelines a play session still built at runtime by
cause: prepared, state not predicted, mesh, shader not in the game files.

`py/learn_priors.py` learns the storage-pointer classifications the oracle uses from the recorded
caches.

`cache-version` prints the pipeline cache version the tool was built with; the precompiled file
carries it in its name and header, and Lichii reads only a file of its own version, so a build
that changes the version needs the file built again (the log names a file of another version).

`validate` compares, for every environment a disk cache recorded, the SPIR-V from the recorded
facts with the SPIR-V from the oracle's facts, and names the fact class behind each difference.
`check` replays every environment of a precompiled file the way `FileEnvironment` does and checks
that each record's key names the code of its environments; with `NX2_CHECK_EMIT=1` it also emits
SPIR-V, and `py/emit_check.py` runs it one environment at a time so a compiler abort is listed
instead of ending the run.

`--priors` is the JSON file `py/learn_priors.py` writes: storage-pointer classifications
(`storage_pointer_direct`, `storage_pointer_indirect`) by stage, constant buffer and offset.

## Files

`PREFIX.envs` (scan): `NXPENV01`, then per program u32 ordinal, u64 shader hash, u32 size and the
serialized environment.

`PREFIX.mesh` (mesh-envs): `NXPMESH1`, then per draw u32 line, u64 mesh state hash, u64 task and
mesh shader hashes, and the sized task (possibly empty) and mesh environments.

`precompiled.fork-v<N>.nx2prep`, in the game's shader directory next to `vulkan.fork-v<N>.bin`:
`NX2PREP1`, u32 cache version, u32 environment count, u32 record count, u32 reserved; per environment
u64 offset and u64 size; per record u8 kind (0 graphics, 1 compute), u8 environment count, u16
reserved, u32 environment indices in stage order, then the 1056-byte graphics or 24-byte compute key.
Environments are stored once and shared by the records that use them.

## Switches

- `EDEN_NX2_PRECOMPILED=0`: skip the preparation step; `1`: run it also on drivers without their
  own pipeline cache.
- `EDEN_NX2_PRECOMPILED_FORCE=1`: prepare again although `precompiled_prepared.fork-v<N>.txt` says the
  file was prepared for this driver and build.
- `EDEN_NX2_EVICT_AFTER_FRAMES=<frames>`: frames a graphics pipeline may go unused before it is
  destroyed (default 9000); `0` keeps every pipeline.
- `EDEN_NX2_PIPELINE_LOG_LIMIT=<n>`: how many runtime pipeline builds over 2 ms are logged (default
  400; builds over 100 ms are always logged).
