# Build 95 pipeline retention experiment

On a Ryzen 7 5800X3D / RTX 5070, disabling graphics-pipeline eviction reduced
return-to-view p99 guest-present intervals in three paired trials by 24–40%.
This dataset records a historical build 95 experiment, not a validated change to
current `nx2`. It complements [#408](https://github.com/zortos293/Lichii/pull/408),
[#550](https://github.com/zortos293/Lichii/pull/550) and
[#555](https://github.com/zortos293/Lichii/pull/555). In particular, #550 merged
before this report was submitted; its behavior was not tested here.

## Results

Each primary window contains 600 intervals. Order was A3, B3, B4, A4, A5, B5
(AB / BA / AB), where A is original eviction and B skips eviction.

| Pair | A p99 (ms) | B p99 (ms) | Reduction | A maximum (ms) | B maximum (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| A3/B3 | 50.07 | 30.09 | 39.90% | 118.77 | 36.17 |
| A4/B4 | 40.48 | 30.66 | 24.26% | 127.95 | 34.34 |
| A5/B5 | 44.62 | 31.32 | 29.82% | 132.77 | 38.54 |

The median across runs of p99 was 44.62 ms for A and 30.66 ms for B; these are
not pooled p99s. Median run p50 was 25.50 / 24.97 ms, and median window FPS was
38.51 / 39.96. The main signal is in the tail, not a large change in usual FPS.
Each A return had one interval above 100 ms; none of the B returns did.

Complete-session logs recorded 2537, 2075 and 2052 pipeline evictions in A3,
A4 and A5, respectively, versus zero in B. These are cumulative totals, not
return-window counts. B logs recorded no runtime graphics-pipeline builds.
There is no per-hash inventory proving that rebuilt pipelines were the exact
ones evicted, nor evidence attributing each long interval to a particular build.

## Environment

- Measured November 7, 2026; game `0400486024DEC000`, Ocarina of Time,
  update v131072, same Kokiri Forest autosave and character position.
- Base commit `222877d4f7c8945b2f31a87b76d09f0a92e4f1b8`, release build 95
  (tag named `build-94`). Both arms used the same instrumented Release binary,
  Clang 22.1.8, `-O3 -DNDEBUG`, Qt 6.11.2. SHA256 is in `results.json`.
- Ryzen 7 5800X3D, 8 cores / 16 threads, performance governor; RTX 5070,
  12227 MiB VRAM reported by NVIDIA, driver 610.57.04.
- Arch Linux, Hyprland Wayland, Qt Xcb; display 3840×2160 at 165 Hz,
  VRR disabled. Internal rendering was Vulkan 1x, Accurate GPU mode,
  asynchronous shaders and presentation enabled, FIFO, frame generation off.
- Each run used a fresh independent copy of the same warmed profile/cache;
  all six loaded 2613 pipelines. Five starting cache-file hashes are recorded
  in `results.json`. The global NVIDIA driver cache was shared and warmed,
  not independently snapshotted. Alternating order does not remove this confound.
- CPU/RSS sampled every two seconds; NVIDIA queried only before and after
  trials, outside measured windows. No builds ran during measurements.

## Reproduce the experiment

Use copies of your own save/profile and legitimately obtained game data.
No game files, saves, keys, screenshots, shader binaries or full user logs are
included here. The historical patch is for an isolated test checkout only:

```sh
# From a clean checkout of the exact base above:
git apply --check /path/to/this/directory/experiment.patch
git apply /path/to/this/directory/experiment.patch
```

Build Release using the project's normal instructions. The patch adds
`LICHII_TEST_KEEP_PIPELINES`: unset/`0` preserves base behavior, `1` skips
`EvictUnusedPipelines`. It also adds opt-in `frame_times(history=true)` to export
the existing raw present timestamp ring without waiting or modifying the render
loop. The historical switch is supplied as a patch artifact, not compiled into
current `nx2`. Skipping eviction is an unbounded diagnostic treatment and must
not be promoted to a production retention policy.

For each trial, launch the same binary with one of these switch values and
`EDEN_NX2_DEBUG_SERVER=127.0.0.1:6547`, using a fresh copy of the identical warmed
profile. Confirm the loaded-pipeline count and visually confirm the game world
is loaded, not the Load confirmation menu. Do not modify the character position.
The tested route faced the Deku Tree from the same Kokiri Forest autosave:

1. Hold `ZL` for 600 presents (view A).
2. Apply `right_stick: [0.6, 0]` for 90 presents, then neutral for 120 (view B).
3. Remain neutral for 17 chunks of 600 presents (10200 total).
4. Hold `ZL` for 600 presents to return to view A. This is the primary window.
5. Remain neutral for another 600 presents, then stop normally.

Submit steps through `input_sequence` using `tools/nx2/debug_mcp.py` from the
patched base. For example, the primary step is:

```json
{"method":"input_sequence","params":{"steps":[{"buttons":["ZL"],"frames":600}]}}
```

For each chunk, record `first_present` and `frames` from the response, submit
three neutral drain presents with `input`, then request
`{"method":"frame_times","params":{"history":true}}`. For a primary window
starting at F, require all 601 IDs F through F+600, and compute each interval as
`timestamp(F+i) - timestamp(F+i-1)` for i=1..600. Reject missing, duplicate or
non-increasing samples. Never interpolate gaps. The history is not an atomic
snapshot: the ring's old overwritten edge can be inconsistent; only the recent,
validated requested slice was used. The potentially pending newest ID is omitted.

Take screenshots outside each measurement window to compare fixed landmarks;
foliage and idle camera height are not pixel-identical. Start from fresh profile
copies and repeat the recorded AB/BA/AB order. A different save or frame rate may
change the camera endpoint, so validate it rather than assume equivalent views.

## Inspect the submitted data

`return-intervals.csv` contains all 3600 primary intervals as integer nanoseconds,
with run, mode, interval index and present ID. Absolute monotonic timestamps and
local paths are omitted. `results.json` contains recorded summaries, seed hashes,
source/binary identity, memory samples and build validation. Run:

```sh
python3 analyze.py
```

The dependency-free analyzer checks indices, present continuity, positive
intervals and recomputes all recorded primary statistics and paired reductions.
The instrumented base passed 18 debug-server cases (178 assertions), 16 pipeline
cases (107 assertions), and 17 Python tests. This documentation PR does not change
runtime code or claim those tests validate a production retention fix.

## Limits and next measurements

There are three repetitions per arm in one scene on one machine, not 3600
independent replications. The intervals measure guest presents, not host scanout,
GPU execution time or end-to-end latency. No statistical significance or
cross-game claim is made.

An initial inverse-camera-rotation protocol drifted; A1/B1/B2 were excluded.
A2 was recenter calibration; an aborted A3 menu attempt was discarded before the
valid A3 trial. Exploratory B2 hung during GUI Stop after measurement and needed
forced termination; the cause was not established. All six final runs stopped
normally. Longer-session memory use and shutdown safety remain unvalidated.

Retention does not remove all stalls: B5 had a 112.95 ms interval during neutral
hold chunk 15, outside the primary window. A3 also had fixed-view gaps of
112.99 and 111.12 ms. A logged CPU read of GPU-written memory preceded one A3
gap by about 3.27 ms, but the log records entry, not duration. A falsifiable next
probe is opt-in timing of `OnCPURead` / `WaitForSyncOperation`, reporting calls
over 30 ms with entry/exit present IDs. Reject this candidate if stalls occur
without an overlapping slow wait.

Retest current `nx2` including #550 before proposing another eviction change.
Any production policy needs bounded memory behavior and longer runs in more
scenes; the historical all-retained arm does not establish such a policy.
