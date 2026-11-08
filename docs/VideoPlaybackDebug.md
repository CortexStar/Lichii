# Debug video playback

Use a separate capture directory for each game and each run. Keep the emulator's
log filter at `*:Info` or include `Service:Info HW.GPU:Info`. Do not enable the
experimental EOS-release override for the first run.

## Capture the failing scene

In PowerShell, before launching the debug build:

```powershell
New-Item -ItemType Directory -Force C:\video-debug\decoded, C:\video-debug\presented
$env:EDEN_NX2_VDEC_TRACE = "1"
$env:EDEN_NX2_VDEC_DUMP = "C:\video-debug\decoded"
$env:EDEN_NX2_FRAME_CAPTURE = "C:\video-debug\presented"
$env:EDEN_NX2_FRAME_CAPTURE_EVERY = "30"
Remove-Item Env:EDEN_NX2_VDEC_EOS_RELEASE_NOW -ErrorAction SilentlyContinue
```

On Linux:

```sh
mkdir -p "$HOME/video-debug/decoded" "$HOME/video-debug/presented"
export EDEN_NX2_VDEC_TRACE=1
export EDEN_NX2_VDEC_DUMP="$HOME/video-debug/decoded"
export EDEN_NX2_FRAME_CAPTURE="$HOME/video-debug/presented"
export EDEN_NX2_FRAME_CAPTURE_EVERY=30
unset EDEN_NX2_VDEC_EOS_RELEASE_NOW
```

1. Launch the emulator from that shell and reproduce the movie without skipping it.
2. Save the complete emulator log and both capture directories.
3. Restart the emulator, select a new capture directory, and repeat while skipping the movie.
4. Record the game ID, build commit, GPU driver, video-decoder setting, and the point where you skipped.

For Yoshi, capture both the title movie and the story intro after New Game.
For Kirby Air Riders, capture from before entering the cutscene through the crash.

## Read the trace

Search the log for `nx2:vdec trace`.

- `submit` records packet size, timestamp, EOS, and the input queue length. Eight queued packets suppress the readiness signal but do not reject a submission. Older builds logged `input full` and rejected packets; Mario Tennis advances past those packets instead of retrying, corrupting VP9 reference frames.
- `decode` records codec, whether FFmpeg opened, pictures produced, the total decoded-frame count, and whether a flush discards those pictures.
- `write` records the output image, CPU address, GPU address, device address, layout, timestamp, and EOS. `gpu_known=false` means no GPU address was established, not that GPU address zero is valid.
- `present` compares the presented image and device address with the latest decoded image write. `same_image` and `same_device` are direct identity comparisons, not proof that the game sampled the video texture. A game can blit the movie into another surface, and a ring can present an older decoded image.
- `release wait`, `handout`, `output`, `output empty`, `flush`, `stop`, `complete fences`, `synthetic EOS`, and `destroy` show the skip/teardown sequence and fence progress.

Decoded PGM captures include the output image ID in their filenames so concurrent
movies do not overwrite one another's captures. They sample the first channel every 120 output frames. For NV12
that channel is luma; for RGBA/BGRA it is one color channel, not a full-color image.
GPU-read block-linear output is captured before swizzling. Presented TGA captures
show what the renderer displays. A nonblack decoded capture beside a black TGA
narrows the failure to the path after decoding, but does not identify the failing GPU pass by itself.

## Compare synchronization and decoder modes

Only after saving the baseline, repeat with `EDEN_NX2_SYNC_SUBMIT=1`.
This enables #123's bounded wait after GPU submissions. It is off by default
because the fork already snapshots command buffers. It can reduce performance
or log a two-second timeout when the GPU needs more guest work.
Unset it or set it to `0` for the normal path.

Repeat with the emulator's video-decoder setting on CPU and then GPU.
Keep the logs separate. AV1 on the CPU needs FFmpeg with libdav1d (`Using decoder
libdav1d`, then `AV1 on the CPU with <n> threads`). AV1 on the GPU needs FFmpeg's
native `av1` decoder and an AV1 hwaccel (the MinGW package from
`tools/windows/build-ffmpeg-mingw.sh`): the log shows `Created vulkan decode device`,
`Using vulkan GPU decoder`, `Using decoder av1` and `decoder for "video/av1" open on
the GPU`. A GPU decoder that fails logs `GPU decoder for "video/av1" failed, decoding
on the CPU`. The `frame <n>` lines count late frames dropped and early replies held.

## Check a decoder against the stream

`EDEN_NX2_VDEC_STREAM_DUMP=<dir>` saves every packet the game submits as one IVF file per
stream (`nx2-vdec-<syncpoint>-<n>.ivf`). With `EDEN_NX2_VDEC_TRACE=1` each written frame logs
`frame hash ... pts=<us> picture=<hash> written=<hash> memory=same`: `picture` is the decoded
frame, `written` the NV12 image built from it (same value when nothing was lost converting),
`memory` whether guest memory holds what was written. Any difference is logged as an error.
Keep the dumps on the PC. Decode one with `tools/nx2/av1_hw_probe.c` and
`PROBE_FRAME_HASHES=1` on `none` (libdav1d), `vulkan`, `d3d12va` and `d3d11va`: its
`frame <n> pts <pts> <hash>` lines use the same hash, so the decoder that differs from
libdav1d, and the frame where it starts, show up by pts.

Unset all trace/capture variables after testing. `EDEN_NX2_VDEC_TRACE` is enabled
by presence, so setting it to `0` does not disable it. Restart after changing
environment variables because the emulator caches them.
