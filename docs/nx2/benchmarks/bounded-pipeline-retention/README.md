# Bounded retention on current nx2: initial paired trial

This is a separate follow-up to the [historical build 95 experiment](../build95-pipeline-retention/README.md).
It compares upstream nx2 `92f0212408f00b459c152c2fb9dcbcf38bcb4ce1` (including #550)
with only the [RAM-target retention policy](../../pipeline-retention.md) added.
Neither arm contains the other changes from #408. One AB pair is a preliminary
check, not a replicated performance conclusion or evidence of significance.

| Arm | p50 (ms) | p99 (ms) | Maximum (ms) | Window FPS |
| --- | ---: | ---: | ---: | ---: |
| Current baseline | 23.01 | 44.93 | 95.74 | 42.54 |
| Bounded-target policy | 22.36 | 29.53 | 31.41 | 44.33 |

Return-window p99 decreased by 34.27% in this single pair. Neither
return contained an interval above 100 ms. Full-session logs recorded 250 evicted
pipelines in baseline and 0 with the new policy; the baseline had already evicted
250 before the return. These counts are not all return-window events.

The policy run still had a 104.36 ms interval during neutral hold chunk 14,
outside the primary window. This change does not remove all fixed-view stalls.

## Conditions and source identity

Same Ryzen 7 5800X3D / RTX 5070, 48,090 MiB MemTotal, NVIDIA 610.57.04,
Arch Linux / Hyprland / Qt Xcb, performance governor. Vulkan 1x, Accurate,
asynchronous shaders/presentation, FIFO, frame generation off, 3840×2160 display
at 165 Hz with VRR off. Clang 22.1.8 Release, `-O3 -DNDEBUG`, WERROR on, LTO
and PGO off; Qt 6.11.2. There were no builds during timed trials.

Both binaries were built from the same code base with identical opt-in history
instrumentation. The first contains only that instrumentation; the second adds
the five source/test/CMake files in policy commit `4edb6ef29`. Both report SCM
`494fde8e481b65369d28874a53f02c0e9c4e113b`, the documentation-only commit on top
of the pinned upstream base: the policy changes were uncommitted at compilation
and subsequently committed with identical contents. Binary hashes distinguish
the arms in `results.json`. This is not an official release binary comparison.

Build 95's v53 cache was not treated as a warmed v54 cache. A private warmup on
the current base loaded the same Kokiri Forest save and cycled the two views
twice before the profile/cache seed was frozen. Both runs loaded 643 pipelines,
from independent copies of seven identical cache files (hashes in results.json).
The global NVIDIA driver cache remained shared and warmed, not independently
snapshotted. AB order and one repetition cannot rule out order effects.

## Protocol and recalculation

Use the same ZL600 / right-stick[0.6,0]90 / neutral120 / 17×neutral600 /
ZL600-return / neutral600 protocol documented in the historical experiment.
The primary window is the 600 return intervals, with screenshots outside each
window used to verify fixed landmarks and character position. Foliage/idle camera
height are not pixel-identical. CPU/RSS sampling ran every two seconds; NVIDIA
queries were outside timed windows. Fresh profiles and the same save were used.

To repeat with your own data, use separate clean checkouts at the reported
baseline and policy commits, apply `history.patch` to both, build with identical
flags, warm the baseline profile, then clone that seed for each arm. The patch
only adds opt-in raw history; it does not add a retention toggle. The actual PR
runtime code excludes this instrumentation. The ring is not an atomic snapshot:
validate the requested recent 601 timestamps as described in the historical
protocol and reject missing/duplicate/non-increasing samples.

`normal` in the CSV means current upstream eviction; `keep` means the new
budget-target policy, not the historical keep-all switch. Integer nanoseconds
are exported without absolute clocks, user paths or game assets. Recalculate:

```sh
python3 ../build95-pipeline-retention/analyze.py --data-dir .
```

## Limits and shutdown

Guest-present intervals do not measure host scanout, GPU execution time or
end-to-end latency. There is one return per arm in one scene, with no workload
large enough to exercise the over-target path in game. Unit tests cover the
count arithmetic and exact oldest selection with ties; they do not establish
long-session memory safety or behavior on other drivers.

The unmodified baseline hung during GUI Stop after its measurement had completed.
Its log recorded `Emulation thread failed to stop`; SIGTERM failed and only that
verified private process was killed. The shutdown failure is retained as a known
limitation; it is not attributed to the new policy. Complete user logs and game
assets are not uploaded.

The policy run exited normally through Stop/Exit after measurement.
