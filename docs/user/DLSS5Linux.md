# Experimental DLSS 5 Neural Rendering on Linux

[DLSS5VKLayer](https://github.com/bmitch87/DLSS5VKLayer) can apply DLSS 5
Neural Rendering to Lichii's completed Vulkan frames. This external tool changes
lighting and material appearance. It is separate from DLSS Super Resolution,
frame generation, and Lichii's [game-DLSS replacement](../native-dlss-replacement.md).
It does not require rebuilding Lichii.

The layer captures the presented image and sends it to a Windows neural-rendering
helper running under Wine or a compatible Proton runner. Lichii itself runs
natively on Linux; Steam is not required. The helper can estimate motion vectors
using NVIDIA Optical Flow instead of receiving the game's engine motion vectors.

This is an experimental third-party setup, not an NVIDIA-supported Lichii
integration. Its output can alter menus, text and artwork as well as the scene.

## Prerequisites

- A Linux x86-64 Lichii build with the Vulkan graphics backend selected.
- An NVIDIA GPU and driver supported by the external layer and neural runtime.
  The test below covers one Blackwell workstation configuration, not every GPU.
- DLSS5VKLayer's Vulkan layer, helper and control tools, installed following its
  [installation instructions](https://github.com/bmitch87/DLSS5VKLayer#readme).
- A compatible Wine/Proton runner configured for the helper. The tested runner
  was Proton-CachyOS; see the layer's documentation for other supported runners.
- A user-supplied `nvngx_dlssnr.dll`, obtained under terms that permit its use.
  Neither this guide nor Lichii provides NVIDIA's proprietary runtime files.

The commands below use Bash and assume the layer's commands are on `PATH`.
For a user-local installation, this may require:

```bash
export PATH="$HOME/.local/bin:$PATH"
```

## Launch

Check the installation and start the helper:

```bash
dlssnr-helper doctor
dlssnr-helper start
```

Resolve any required dependency errors before continuing. Launch Lichii with the
layer enabled for that process, replacing the path with your AppImage:

```bash
VKLayer_DLSS5=1 /path/to/lichii-linux-x86_64-pgo.appimage
```

Open a game. The layer skips very small startup surfaces, so the helper may wait
for frames until a game starts. To adjust settings, run `dlssnr-gui` in another
terminal. In the tested version, closing that GUI requests a helper shutdown;
leave it open while using its controls.

## Starting settings and comparison

A useful starting point from the test below is one **Natural** pass at full
working resolution, with estimated motion vectors enabled. Two passes also
initialized, but the tester preferred one. Additional passes are not a guarantee
of better quality and require more processing.

The following commands apply those settings to the running helper. For the
tested user-local installation, the CLI is at the path shown below; replace it
with your installation's `dlssnr-shmctl` path if necessary. If using the GUI,
open it before these commands because it loads saved settings when it starts.

```bash
DLSSNR_CTL="$HOME/.local/lib/dlssnr/bin/dlssnr-shmctl"
DLSSNR_SHM_PATH="/tmp/dlssnr-$UID/shm.bin"
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set enabled 1
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set style 1
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set passes 1
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set workingscale 1
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set mvec 1
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set applymodel 1
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set bypass 1
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set debugview 0
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set compare 0
```

Here `bypass=1` selects the model's raw output rather than the layer's edit
composition; it does not disable neural rendering. These are live controls;
do not assume CLI changes persist across helper or GUI
restarts. The shared-memory path above is the layer's default. If you configured
a custom path, use that path instead. Settings affect other applications sharing
the same helper, so test one application at a time.

For a centre-screen comparison wipe, then a return to the full enhanced image:

```bash
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set compare 2
# After comparing:
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" set compare 0
```

Compare both a stationary scene and camera movement. Look for changes to text,
fine foliage, particles and moving objects, and assess frame pacing as well as
appearance.

## Verify processing

```bash
dlssnr-helper status
"$DLSSNR_CTL" "$DLSSNR_SHM_PATH" status
```

A running helper alone does not prove neural rendering is active. While the game
is presenting frames, check for `model_up=1`, `layer_composition_up=1`, and
increasing `helper_frames` and `layer_frames` on successive status calls. Check
`settings` for `enabled=1`, `applymodel=1` and `debugview=0`, then verify a visible
change with the comparison control.

The helper log is normally
`${XDG_STATE_HOME:-$HOME/.local/state}/dlssnr/helper.log`. Successful initialization
in the tested version logged `Feature=18 created=true`, `neural ready`, and
`first optical-flow pass completed`. With `workingscale=1`, the reported neural
size matches the presentation image, which may differ from the game's internal
render resolution.

## Troubleshooting and disabling

- **No effect:** check `doctor`, the active settings and the counters above.
  Confirm Lichii uses Vulkan and was launched with `VKLayer_DLSS5=1`. A missing
  helper or failed neural initialization can leave the original image visible.
- **Model initialization fails after resizing/fullscreen:** inspect the helper
  log. In the test below, changing to 3840 × 2160 failed while both GPUs were
  nearly full; memory pressure was suspected but not established as the cause.
  Return to a previously working window size. A temporary
  `set workingscale 0.5` reduces the neural pass width and height without changing
  Lichii's display resolution. Restart a failed helper with
  `dlssnr-helper restart`, reapply the desired live settings, and verify that
  counters advance. Full working resolution was subsequently restored at
  2400 × 1350.
- **UI changes or motion artifacts:** this setup processes the completed image
  and uses estimated motion. It has no validated game-specific HUD separation.
  Disable the effect to compare before reporting an emulator rendering bug.

To disable processing live, use `set enabled 0`. When finished testing, stop the
helper with `dlssnr-helper stop` if no other application is using it. For an
explicitly unmodified launch, close Lichii normally and run:

```bash
DLSSNR_DISABLE=1 /path/to/lichii-linux-x86_64-pgo.appimage
```

## Tested configuration and limits

Manual testing on 8 November 2026 used:

| Component | Configuration |
| --- | --- |
| Lichii | Linux x86-64 PGO AppImage, log version `nx2-build-117` |
| Game | The Legend of Zelda: Ocarina of Time |
| OS / driver | CachyOS, NVIDIA 615.78.08 |
| GPUs installed | Two RTX PRO 6000 Blackwell Workstation Edition GPUs, 96 GB each |
| External layer | Local 64-bit DLSS5VKLayer 0.3.1-2 build, commit `9f43793d354063399b463a2a20adf30ce07e1c60` |
| Helper runner | Proton-CachyOS (`proton-cachyos-slr`) |
| Preferred settings | Natural, one pass, full working resolution at 2400 × 1350 |

Two installed GPUs do not imply that this setup distributes neural processing
across both. Initialization logs, advancing frame counters and matched title-screen
captures confirmed processing and visible changes to foliage, rocks and lighting.
The title logo also changed. The tester reported satisfactory appearance and
preferred one pass after trying two.

The 4K resize failure remains unresolved. No controlled FPS/latency benchmark,
long-duration stability test, or broad game/GPU compatibility test was performed.
For reports, include the exact versions, presentation size, layer settings and
whether the issue reproduces with the layer disabled. Review logs for personal
paths before sharing them. Report helper/layer failures to
[DLSS5VKLayer](https://github.com/bmitch87/DLSS5VKLayer/issues); report issues that
also reproduce without it to Lichii.
