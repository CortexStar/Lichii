# Experimental NV cooperative-matrix lowering

The HMMA.16816.F16 path requires `EDEN_NX2_TENSOR=fast` at startup. It is
not enabled by default and is not a claim of numerical equivalence across
GPU architectures or drivers. `EDEN_NX2_TENSOR=stub` retains the existing
diagnostic D=C mode; it is not matrix multiplication.

The renderer admits the cooperative path only when all of these hold:

- `VK_NV_cooperative_matrix` and its `cooperativeMatrix` feature are enabled.
- The device advertises the subgroup-scoped M=16, N=8, K=16 operation with
  Float16 for A, B, C, and D, in the compute stage.
- `shaderFloat16` is still enabled after the renderer's driver workarounds.
- The renderer can emit SPIR-V 1.4 or newer.
- Subgroup size control can request 32 lanes in compute, full compute
  subgroups are enabled, and basic compute subgroup operations are supported.

The existing NVIDIA Ampere-and-newer Float16 workaround is not bypassed.
In particular, the previously investigated RTX 3080 is **not eligible**
through this F16-scratch NV path. KHR-only devices are also ineligible.
No currently tested device/driver is numerically qualified by this change.
The earlier KHR prototype's results do not qualify this implementation.

The build uses the pinned, source-patched Sirit package. Prebuilt Sirit
archives cannot acquire the new NV instruction methods from a source patch.
The upstream v1.0.5 revision, archive hash, license, and
`SIRIT_USE_SYSTEM_SPIRV_HEADERS` option are unchanged. Windows release and
Android builds now compile Sirit's small C++ library instead of downloading
its prebuilt archive; this adds work to the first build, but unchanged sources
are reused by incremental builds. The source path was already used by other
platforms and Windows debug builds.
The source patch also removes an upstream linker-only flag from Sirit's
compile options, which otherwise fails a strict clang-cl Release build.
An isolated two-job Release build of this dependency took 11.72 seconds with
clang-cl and 11.46 seconds with MSVC on the validation machine; incremental
builds did no compilation. These are dependency-only timings, not full-emulator
build results. Both compilers used the real CPM fetch and patched sources;
reconfigure left all 44 Sirit source files unchanged without reapplying patches.
Isolated dependency builds also passed with Linux GCC 13 and Clang 18,
MinGW x64, and Android arm64 using NDK r28c. Native Linux encoding checks
passed; the cross-built executables were not run. These checks do not establish
full-emulator build success. macOS dependency validation remains with CI.

Native lowering accepts only the dense, unpredicated encoding and aligned,
nonreserved register spans. The SPIR-V backend checks admission again,
including shaders loaded from disk: collectives must precede any structured
selection, loop, early return, or other control-flow node; workgroups must be
one-dimensional, with 32–1024 invocations in multiples of 32.
The invocation, X-dimension, and subgroup-count limits of the device also apply.
These restrictions deliberately reject programs the backend cannot prove safe.

Scratch is allocated only for programs containing tensor instructions, at
1,280 bytes per subgroup. Plain Workgroup variables have separate storage.
With explicit Workgroup layout, scratch starts at a 16-byte-aligned offset
beyond the guest's shared memory, with the required alias and 16-bit-access
declarations. If 16-bit explicit-layout access is unavailable, both guest
shared memory and scratch use the non-Block path instead. The combined
allocation must fit the device limit. All scratch barriers have Subgroup
execution and memory scope.

Unavailable hardware, disabled opt-in, and rejected compute admission retain
the existing native matrix-multiply upscale fallback. Runtime admission
failures remain cached by the full compute key, so subsequent dispatches
cannot silently skip that fallback. Disk replay skips rejected tensor entries.
The Vulkan and OpenGL cache versions are advanced to discard previously
lowered tensor programs; Vulkan also discards the invalid compiled modules.

The focused tests are in `src/tests/video_core/cooperative_matrix.cpp`
(`[tensor]`). Setting `EDEN_TENSOR_TEST_OUTPUT` to a scratch directory writes
the actual backend-generated modules for independent `spirv-val` checks.
They verify admission, opcode emission, subgroup barriers, scratch placement,
shape matching, and fallback-compatible rejection. They do not execute matrix
arithmetic on a GPU; native-output comparison remains required before
qualifying a device or enabling this path by default.
