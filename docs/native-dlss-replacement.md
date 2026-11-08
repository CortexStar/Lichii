# Game-DLSS replacement

The Vulkan renderer can replace a recognized game-DLSS evaluation with NVIDIA DLSS on
Windows, then AMD FSR 3.1.4 if NVIDIA DLSS is disabled or unavailable. FSR uses AMD's
eight temporal-upscaling passes, including depth, motion, history and optional RCAS.
This is separate from the presentation filters that scale the finished window image.

Fire Emblem uses the jitter-only spatial workaround by default. Explicit `fsr` and `dlss`
comparison modes enable temporal replacement for its validated input contract. The upstream
temporal blend and RCAS fallback remain available if a temporal backend is unavailable.
Invalid consumed inputs leave the native dispatch enabled. An already-rejected native
tensor shader remains an explicit dispatch failure. General tensor emulation remains.

## Supported input contract

Selection currently recognizes Fire Emblem: Fortune's Weave's captured prepass and output
shader, under its exact program ID. Temporal mode requires matching B10G11R11 input/output,
R16G16 motion, D32 depth and a fresh jitter sample. Spatial mode requires only the matching
color/output and fresh jitter. The upstream temporal contract uses reversed depth,
normalized motion toward the previous frame, HDR color and jitter in render pixels.
Resolution scaling scales the render extent and jitter together. History resets on size
changes, first use and evaluation gaps longer than 250 ms. An explicit game camera-cut
reset is not yet extracted.

Fast Fusion's shaders still execute natively. Its captured shader identities are known,
but motion scale/direction, jitter units, depth convention, reset and resource roles have
not been proven for this LLE path. The RGBA16F menu/DLAA path also requires alpha handling
that the Fire Emblem contract does not provide. Turning on NVIDIA DLSS does not override
these safety checks and is not evidence of a Fast Fusion visual fix.

For a diagnostic run, `EDEN_NX2_DLSS_CONTRACT_TRACE=1` logs at most 24 matching Fast Fusion
dispatches per compute engine. `EDEN_NX2_DLSS_CONTRACT_SKIP=N` skips the first N matching
dispatches, capped at one million. Logs include exact shader identity, QMD launch dimensions,
bounded constant-buffer words and raw texture descriptors. They contain guest addresses
and should remain private. No shaders are skipped or game files changed by this capture.

## Ocarina of Time

The Legend of Zelda: Ocarina of Time (`0400486024DEC000`) has no DLSS of its own. It renders its
3D, tonemaps it, and runs a single-frame anti-aliasing network with a sharpening output pass.
"Ocarina of Time anti-aliasing" set to NVIDIA DLSS (4K Quality) or NVIDIA DLAA (4K) replaces
that network with NVIDIA DLSS ("Use NVIDIA DLSS" on, Streamline with DLSS for the device). Both
set the game's resolution scale to 1.5x (4K output). Implementation: `video_core/oot_dlss.h`,
the hooks in `vk_rasterizer.cpp`, `KeplerCompute::ReplaceOotNetwork`, `TextureCache::OotDlss`,
`vk_oot_motion_vectors.cpp` and `vk_oot_object_motion.cpp`; the design and measurements are in
docs/nx2/oot-motion-vectors-plan.md.

- **Jitter:** the viewport of every scene-geometry draw (into the scene depth, testing or writing
  it) gets a Halton sub-pixel offset. Full-screen passes are not jittered, nor anything drawn
  after DLSS ran in the frame.
- **Motion vectors:** camera motion is rebuilt from the scene depth and the game's current and
  previous view-projection, with a 0.35-pixel dead zone so a still camera does not wobble.
  Per-object motion comes from vertex history for vertex pipelines and from re-running mesh
  programs on last frame's constants (`ir_opt/oot_shadow_pass.cpp`); draws that can do neither
  keep camera motion and are logged once with the reason.
- **DLAA (4K):** the 3D renders at 3840x2160 and DLSS runs on the linear HDR scene colour before
  the tonemap, with the tonemap's own exposure (its constant buffer 3) as DLSS's exposure.
  Bloom, the tonemap and the HUD then work on DLSS's result.
- **Quality (4K):** the 3D stays at the game's 2560x1440 (only the images after it are
  rescaled) and DLSS upscales the tonemapped image in place of the network.
- **Particles:** fireflies, dust and fairy glow are transparent draws after the fog composite
  that don't write depth. They have no motion vectors and trailed in DLSS's history, so DLSS runs
  before them: at the first such draw in DLAA; in Quality the tonemap draw is repeated from a
  copy of the scene colour taken there, DLSS upscales that, and what the particles added to the
  tonemapped image is added to its output. The opaque draws after the fog composite (water
  surfaces) stay in DLSS's input.
- **Textures:** NVIDIA's LOD bias for the render-to-output ratio (log2(render / display) - 1:
  -1.585 in Quality, -1 in DLAA), and a chosen anisotropic level also covers LOD-clamped
  samplers.
- **Sharpening:** DLSS Sharpness (AMD RCAS on DLSS's output) applies to Quality only.
- **Fallback:** a frame DLSS cannot evaluate copies the tonemapped image in place of the network
  (no sharpening). Every 600 frames the log counts the reasons
  (`OoT DLSS: fallbacks to a copy in the last 600 frames: ...`).

### AMD FSR 4 and AMD FSR 3.1 (Ocarina of Time)

The two AMD modes feed the same jitter, motion vectors, masks, exposure and particle handling to
AMD's upscalers instead of DLSS, on any vendor's build (no Streamline):

- **AMD FSR 4 Native AA** (`AmdFsrNativeAa`): like DLAA, the 3D renders at the resolution scale
  and FSR 4 anti-aliases it 1:1 on the HDR scene colour before the tonemap. At 1x this is the
  game's own size (2560x1440 docked), which fills a 1440p display's 16:9 area and also fits a
  native 3440x1440 render: the output is always the network's output image at the scale,
  nothing is hard-coded to 4K.
- **AMD FSR 4 upscaling** (`AmdFsrUpscale`): like DLSS Quality, the 3D stays at the game's size and
  FSR upscales the tonemapped image by the resolution scale (1.5x unless a higher scale is set;
  the scale is the upscale ratio).
- **Backends:** AMD FSR 4 through AMD's runtime DLLs ("AMD FSR runtime folder", see
  `vulkan_common/amd_ffx.h`) when it offers FSR 4 for the GPU; the provider picks the model
  (FP8 on Radeon RX 9000). Its context is created on a worker thread; until it is ready, and
  without the DLLs, the in-tree AMD FSR 3.1 runs with PR #695's OoT profile (tonemapped colour,
  finite standard depth near 0.1 / far 25000, unit exposure; HDR input is then off, see
  `OotDlss::HdrInput`). `EDEN_FFX_UPSCALER=<version name part>` pins an FSR version.
- **Sharpening:** AMD's own pass, set by the RCAS sharpness slider (`fsr_sharpening_slider`, 0%
  off): FSR 4's sharpening or FSR 3.1's RCAS. Lichii's RCAS on top (DLSS Sharpness) is not used.
- **Masks:** the effects/fog bias mask (fairy glow, fireflies, dust, fog) goes to FSR 4 as its
  reactive mask and to FSR 3.1 as reactive and transparency/composition masks. Particles drawn
  after the upscaler (the tonemap redraw path) never enter its history.
- **LOD bias:** AMD's log2(render / display) - 1 (super-resolution-ml.md "Mipmap biasing"), with
  the resolution scale as the ratio in the upscaling mode, -1 in Native AA.
- **HUD:** the game draws its HUD after the network, on the upscaler's output, so text never goes
  through FSR.

### AMD FSR frame generation and the frame cap

- **Frame generation** "AMD FSR Frame Generation" (`frame_gen_type` AmdFsr) presents through
  AMD's frame generation swapchain for Vulkan (`present/amd_frame_gen.cpp`): AMD's runtime
  generates one frame per real frame inside present, on its own async compute queue, and paces
  the presents itself. On RDNA 4 with Adrenalin 26.10.1+, borderless/exclusive fullscreen, VSync
  off (the swapchain is IMMEDIATE) and presents inside the VRR window, AMD's driver meters the
  flips (Driver-Assisted Frame Pacing, OptiScaler's "Mode 2"; notes/fg-hwflip.md). It takes its
  inputs (depth, motion, jitter, camera from the view-projection, `video_core/oot_camera.h`)
  from Ocarina of Time's upscaler path, so it needs one of the DLSS/AMD anti-aliasing modes;
  other frames are presented without generation. Lichii presents on the GPU thread then, in
  FG 4.0.2's configure, prepare, present order. The log says which pacing applies and why;
  the debug server's `present_status` reports the same.
- **Frame cap:** with "Variable Refresh Rate Display" on, AMD frame generation holds the game at
  half of floor(R - R^2/3600) (112 at 240 Hz, so 224 shown, inside the VRR window that driver
  pacing needs) through the nx2:vi2 present pacer (`ChooseAmdFrameGenPolicy`, `HostFrameCap`).
- **Frames start from the display:** with present waits (VK_KHR_present_wait2) or AMD's frame
  generation swapchain, the nx2:vi2 vsync event the game starts its frames on is signalled by
  the presenter when the frame before the last reaches the display (or AMD's swapchain takes a
  present), not by a fixed timer (`video_core/host_vsync.h`). Starts are held to the pacer's
  rate: the vsync rate, the FPS limit, the VRR cap, and the movie cadence
  (`DisplayVsyncDeadline`). The timer steps in only when the display stops signalling
  (`TimerVsyncSignals`). `EDEN_NX2_HOST_VSYNC=0` keeps the timer for A/B runs.
- **Anti-Lag 2** (`anti_lag`, VK_AMD_anti_lag): its input stage runs right before that signal on
  the presenting thread, a host thread, so the guest's cores never block there. It holds the rate
  as maxFPS in place of Lichii's wait. The present stage, with the same frame index, runs right
  before that frame's present.
- **Latency:** guest frame start to host present is logged every 600 frames and reported by
  `present_status` (mean, p50, p99, max). Judge pacing on PresentMon's MsBetweenDisplayChange:
  with the driver metering flips, present-call intervals alternate (OptiScaler's "thick" graph)
  while display intervals are flat. PresentMon can't tell generated from real frames on Vulkan.

What to look for in captures, A/B against Game and NVIDIA DLAA where available: shimmer on
Kokiri Forest's grass and leaves during a slow pan; ghosting behind Link when he runs across a
textured wall; trails behind Navi and the fireflies at night; stability of thin fences and
ropes; HUD and dialogue text edges; water edges.

Shader hashes, program keys and constant buffer offsets are those of the game's current build;
a game update needs them re-checked. While either DLSS mode is selected, No sharpening
(docs/nx2/oot-network-bypass.md) steps aside: DLSS replaces the same output pass.

Switches for comparison runs (all off unless set): `EDEN_OOT_DLSS=1` (DLSS without the setting),
`EDEN_OOT_DLSS_OUTPUT=dlaa|quality`, `EDEN_OOT_OBJECT_MOTION=0`, `EDEN_OOT_DLSS_HDR=0` (DLAA after
the tonemap), `EDEN_OOT_DLSS_BEFORE=fog|tonemap` (where DLSS runs on the HDR path),
`EDEN_OOT_DLSS_EXPOSURE=0` (DLSS's auto exposure), `EDEN_OOT_LOD_BIAS=<n>`,
`EDEN_OOT_CAMERA_DEADZONE=<px>`, `EDEN_OOT_DLSS_MASK_GAIN=<n>`, `EDEN_OOT_DLSS_FLIP_Y=0`,
`EDEN_OOT_MOTION_CHECK=1` (measures the motion vectors' reprojection error),
`EDEN_OOT_MOTION_DEBUG=1` (shows the motion vectors) and `EDEN_OOT_CAMERA_TRACE=1`.

## Settings and comparison runs

"Use NVIDIA DLSS in place of a game's DLSS" defaults to off and takes effect when the game
starts. Disabled means no NVIDIA runtime is loaded; an explicitly selected temporal
replacement uses FSR 3.1. Missing DLLs and unsupported GPUs fall back to FSR. Recording is
asynchronous. A failed DLSS recording clears that frame's output, restores input layouts,
and makes later evaluations fall back; it does not wait for the scheduler worker.

`EDEN_NX2_DLSS_REPLACEMENT=fsr` selects temporal replacement and forces FSR for comparison.
`dlss` selects temporal replacement with NVIDIA preferred when enabled and available.
`native` preserves the guest evaluation. With no override, Fire Emblem uses spatial de-jitter.
The old `EDEN_NX2_DLSS_STANDIN` variable is not read. The internal `standin` backend override
does not select Fire Emblem's intercepted dispatch. The RCAS sharpness setting controls FSR
and fallback sharpening. NVIDIA model-selection overrides
from the upstream implementation remain available for diagnostic comparisons.

## Build and distribution

FSR needs no external runtime. Keep `externals/FidelityFX-SDK/LICENSE.txt` and its attribution
with distributions. The vendored files are unmodified AMD MIT sources from FidelityFX SDK
v1.1.4, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.

For the optional Windows NVIDIA path, obtain only the MIT Streamline headers and license
from NVIDIA-RTX/Streamline tag `v2.14.16`, commit
`625db07d4bca5abc67d781c42af7e7fb0761f1b6`. The SDK directory must contain `include/sl.h`,
`include/sl_dlss.h`, `include/sl_helpers_vk.h`, `include/sl_security.h` and their companion
headers. MSVC x86_64 builds enable `ENABLE_NVIDIA_DLSS` by default and fetch the headers;
`-DEGLANTINE_STREAMLINE_SDK=<header-root>` selects a local copy. This adds Windows
`version`, `wintrust` and `crypt32` libraries, not NVIDIA import libraries. Retain Streamline's
MIT `license.txt` in the packaged notices.

Do not bundle NVIDIA's proprietary DLLs. The Graphics panel obtains missing files from
NVIDIA's Streamline release only after the user reviews and accepts the license texts.
The runtime searches `EDEN_STREAMLINE_DIR`, the user `streamline` directory, then the
executable's `streamline` directory. It verifies the interposer signature and supports the
NVIDIA app's signed DLSS model. The UI and Windows loading path require Windows validation.

## Provenance

This backport is AI-assisted and requires human review and testing before distribution.
It carries upstream work by Duck912 from Eglantine commits `08dbcc7e3b3d`, `11cebf56014a`
and PR #434 through `f9d0c802e8e4`, plus Jonas's warning fix `537c1f4e173a`. The empty-extension requirement
fix `1889602c7a0d` is a separately included follow-up. Existing SPDX notices and AMD/NVIDIA
license text are retained. The upstream nx2 merge also carries newer asynchronous recording
and fallback changes while retaining the fork's signature checks and input validation.
