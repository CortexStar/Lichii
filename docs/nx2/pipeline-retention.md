# Switch 2 graphics pipeline retention

Graphics pipelines that have not been used for more than 9000 GPU frames are
eligible for eviction. Rebuilding them when an area returns can stall draws even
when the driver pipeline cache is warm.

For Switch 2 titles, retain eligible pipelines while the graphics cache is within
a count target derived from one sixteenth of physical host RAM, using an estimate
of 450,000 bytes per pipeline measured on NVIDIA. For example, 16 GiB gives a
target of 2386 pipelines and 32 GiB gives 4772. Above the target, remove only the
excess eligible pipelines, oldest first. Equal last-use frames do not cause extra
pipelines to be removed or newer ones to be selected ahead of older ones.

The existing exclusions remain: current, recent and unbuilt pipelines are not
selected, and up to 2000 stale prebuilt pipelines that no draw has used are
protected. Removed objects remain in the existing retirement queue until the GPU
scheduler permits destruction, and surviving pipelines have transitions cleared.
MoltenVK retains its existing no-eviction policy. Switch 1 keeps its existing age
policy and prebuilt protection.

This is a heuristic **count target**, not a hard RAM or VRAM limit. Driver memory
cost varies, and protected/recent pipelines and pending retirement can keep memory
and cache counts above the target. It does not react to current available host
memory or guarantee that a workload fits in memory.

The budget policy is adapted from ZeroOneTV's commit
[`4b65b8f8`](https://github.com/zortos293/Lichii/commit/4b65b8f8dcc8274d8e9b1f02c84e6021f3ec33e5)
in [#408](https://github.com/zortos293/Lichii/pull/408). Exact oldest selection and
cutoff-tie handling are adapted from Jordan's
[#313](https://github.com/zortos293/Lichii/pull/313). This isolates retention from
other shader, texture and synchronization changes, restricts the new target to
Switch 2, and preserves the prebuilt protection integrated in
[#550](https://github.com/zortos293/Lichii/pull/550).

The [historical build 95 experiment](benchmarks/build95-pipeline-retention/README.md)
measured the motivation: skipping all eviction improved return-window p99 in three
pairs on a Ryzen 7 5800X3D / RTX 5070. That experiment used an unbounded diagnostic
treatment; its 24–40% reductions must not be attributed to this bounded-target
implementation or to current `nx2` without a new comparison. Independent fixed-view
stalls remain a separate investigation.

An [initial current-base paired trial](benchmarks/bounded-pipeline-retention/README.md)
compares this implementation against nx2 including #550 with equal warmed cache
state. It is reported separately from the historical keep-all experiment.
