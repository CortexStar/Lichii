# Shader precompile from game files (nx2 shaderwarm)

Builds what a Switch 2 game's shaders need before the game is played, from the game files alone
(no recordings), so new content stutters less. Everything is off unless an environment variable
or a precompiled `vulkan_ir.bin` turns it on; off takes the old code paths.

## What it does

A native (NVN2) graphics pipeline build costs, in order: lowering (small), CFG + translation of
each stage (GPU thread), SPIR-V, then the driver compile (on a worker; the draw waits for it unless
asynchronous shaders skip it). Two parts remove the stage-dependent work from that path:

1. **Per-stage translation cache** (`<title cache dir>/vulkan_ir.bin`). A stage's translation
   depends only on its own program and its environment answers, not on the pipeline key, so it is
   cached per stage: the IR after translation plus the answers it used. At run time a stage whose
   record's answers match the live environment (storage pointers, texture queries re-asked with
   the live handles, texture bound buffer, viewport) skips CFG + translation. Records exist for
   vertex/pixel stages, compute programs (only for their own work group when their storage writes
   have a bounded extent) and, opt-in, task/mesh stages.
2. **Graphics pipeline libraries** (`EDEN_NX2_GPL=1`, VK_EXT_graphics_pipeline_library with fast
   linking). Native vertex+pixel pipelines are linked from four libraries (vertex input,
   pre-rasterization, fragment shader, fragment output); the two stage libraries depend on the
   stage's SPIR-V and a few driver-relevant fields only, so a precompiled stage library is a driver
   disk cache hit at draw time and the link costs microseconds. An optimized link replaces the fast
   one in the background. Library pipelines are made without a render pass: in this mode every 3D
   draw is recorded with dynamic rendering. Per-stage descriptor sets (vertex 0, fragment 1).

The **precompile** (`EDEN_NX2_PRECOMPILE=<pack>`) fills both while the game boots: a pack holds
program blobs found in the game files (a scan for program headers, plus header-less compute
programs with their launch from NVN control sections). Each stage is lowered and translated with
default answers (2D float textures, mapped storage pointers, the program's own constants),
verified (the cached form gives the same SPIR-V), appended to vulkan_ir.bin, and, with GPL, its
library is created once so the driver's disk cache has it. Compute programs with a known launch
are built once as pipelines (driver cache) and dropped.

## Files

| file | contents |
|---|---|
| `video_core/nx2_ir_cache.{h,cpp}` | vulkan_ir.bin format, IR (de)serialization |
| `video_core/nx2_precompile_env.{h,cpp}` | the environment a program from a game file is translated with |
| `video_core/renderer_vulkan/vk_shaderwarm.{h,cpp}` | cache lookup, precompile, pack reader, library stage emission (PipelineCache members) |
| `video_core/renderer_vulkan/vk_gpl.{h,cpp}`, `vk_gpl_pipeline.cpp` | library cache, per-library state, linking (GraphicsPipeline members) |
| `video_core/renderer_vulkan/vk_scheduler_rendering.cpp` | dynamic rendering instances |
| `video_core/renderer_vulkan/nx2_pipeline_stats.h` | `EDEN_NX2_PIPELINE_STATS` |
| `shader_recompiler/frontend/ampere/nx2_relative_lepc.h` | address-independent geometry LEPC (opt-in) |

Hooks in existing files are marked `nx2 shaderwarm`.

## Environment variables

| variable | default | effect |
|---|---|---|
| `EDEN_NX2_PRECOMPILE=<pack>` | unset | precompile the pack into vulkan_ir.bin (creates it) while the game boots |
| `EDEN_NX2_PRECOMPILE_ONLY=1` | off | the boot builds no pipelines of its own (a precompile run, nothing else in the caches) |
| `EDEN_NX2_PRECOMPILE_THREADS=<n>` | cores/2 | precompile threads (low priority) |
| `EDEN_NX2_PRECOMPILE_COMPUTE_BUILD=0` | on | don't build compute pipelines for the driver cache |
| `EDEN_NX2_PRECOMPILE_VERIFY=0` | on | skip the cached-form SPIR-V check |
| `EDEN_NX2_PRECOMPILE_TRACE=<file>` | unset | "begin/end <source>" per stage (finds a program that crashes the recompiler) |
| `EDEN_NX2_IR_CACHE=0` | on if the file exists | ignore vulkan_ir.bin |
| `EDEN_NX2_IR_CACHE_COMPUTE=0` | on | no compute records |
| `EDEN_NX2_IR_CACHE_TASKMESH=1` | off | task/mesh records (one precompile run with them stalled; unexplained) |
| `EDEN_NX2_IR_CACHE_CHECK=1` | off | also translate each hit live and compare the SPIR-V |
| `EDEN_NX2_GPL=1` | off | graphics pipeline libraries + dynamic rendering |
| `EDEN_NX2_GPL_ATTRS=0` | on with GPL (RADV) | vertex stages keep typed attributes (no VK_EXT_legacy_vertex_attributes) |
| `EDEN_NX2_GPL_LTO=0` | on with GPL | keep fast-linked pipelines (no optimized link) |
| `EDEN_NX2_GPL_CHECK=1` | off | log which SPIR-V inputs of a library stage differ from the precompile's |
| `EDEN_NX2_GPL_DUMP=<dir>` (+ `_ONLY=<hashes>`) | unset | write each library stage's SPIR-V and library state, at run time (`-rt`) and in the precompile (`-pc`) |
| `EDEN_NX2_RELATIVE_LEPC=1` | off | address-independent geometry LEPC + geometry records |
| `EDEN_NX2_PIPELINE_STATS=1\|2` | off | phase timings and counters every 10 s (`2`: per pipeline) |
| `EDEN_NX2_PIPELINE_LOG=1` | off | one line per runtime graphics pipeline |
| `EDEN_NX2_ASYNC_SHADERS=0\|1` | setting | override use_asynchronous_shaders |
| `EDEN_NX2_MESH_INIT=0` | off | zero SET_MESH_INIT_SHADER for mesh pipelines without a task |

## Precompile a game and play it

The tooling lives outside the tree (s2-shaderwarm `tools/s2warm`): it scans the game file into a
pack and boots the game headless with `EDEN_NX2_PRECOMPILE`.

    EMU_DIR=<unpacked build> STEAM_APPID=<shortcut id> tools/s2warm <game.nup> <portable data dir>

- `vulkan_ir.bin` lands in `<data dir>/user/cache/shader/<title id>/`. It applies only to the build
  and driver that wrote it (tag: format, git rev, Info/Inst sizes, driver id + version, GPU); after
  a rebuild or a driver update it is ignored and the precompile must run again.
- The stage libraries and compute pipelines land in the driver's disk cache: the run must use the
  same `MESA_SHADER_CACHE_DIR` and format as play. A Steam shortcut gets
  `MESA_SHADER_CACHE_DIR=~/.local/share/Steam/steamapps/shadercache/<appid>`,
  `MESA_DISK_CACHE_SINGLE_FILE=1`, `MESA_SHADER_CACHE_MAX_SIZE=5G`; `STEAM_APPID` makes s2warm use
  the same.
- Play with `EDEN_NX2_GPL=1` to use the libraries; the translation cache needs nothing.

## Limits

- Library pipelines: native vertex+pixel only (no tessellation, geometry, mesh, transform
  feedback, legacy varyings, fragment inputs the vertex stage doesn't write); their SPIR-V is
  emitted on the GPU thread. The library vertex stage declares attributes as float on RADV
  (VK_EXT_legacy_vertex_attributes); elsewhere integer attributes miss the precompiled library.
- Default answers: a stage whose live answers differ (integer texture formats, unmapped storage
  pointers) is translated as before.
- With `EDEN_NX2_GPL=1` render passes are replaced by dynamic rendering for 3D draws; blits keep
  theirs. Tilers with resolve attachments keep render passes.
- Another graphics-pipeline-library prototype in the same tree (daniel-lynch/Lichii
  `feat/pipeline-library`, `EDEN_NX2_PIPELINE_LIBRARY=1`) declares the same device members
  (`graphics_pipeline_library` feature/property, `pipeline_library` extension) and patches
  `GraphicsPipeline::MakePipeline`: merging both needs one set of device members, the extension
  enabled when either variable asks, and only one of the two at run time (its parts are made with
  render passes; this mode draws with dynamic rendering). That line also has its own translation
  cache; the two caches don't share files.

## Putting it on another build

The stack is four commits (shader_recompiler hooks, vulkan_common, video_core, this doc). New files
are the same for every base; the hooks exist in two flavors, Lichii-based (`lichii` binary) and
plain Eglantine (`eglantine`). In s2-shaderwarm, `tools/apply-shaderwarm.sh <branch>` makes a
worktree on a new branch, cherry-picks the matching flavor, stops with the list of files on a
conflict (the hooks are marked `nx2 shaderwarm`), builds and smoke-tests it.
