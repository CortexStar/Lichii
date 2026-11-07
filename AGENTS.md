# AGENTS

This file is policy and routing, not a knowledge base. The knowledge lives in `docs/` — read the
relevant doc before working in an area, and write durable findings back into docs (see
[Docs map and maintenance](#docs-map-and-maintenance)). `docs/Coding.md` has the technical coding
guidance.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Know the machine you are on

Never assume the environment — detect it, then adapt:

- OS, shell, and package manager first (`uname`, `$env`, `which`). Commands quoted in docs are
  portable unless a section says otherwise; paths like `C:/lx/...` belong to one specific machine
  (see [The owner's PC](#builds)) and do not exist anywhere else.
- CPU core count sets sane build/test parallelism; free disk and RAM set whether a full configure or
  an object-level check fits. Probe (`nproc`, `free`, `df`) instead of guessing.
- GPU vendor, model and driver change graphics diagnosis completely. Pull them from the user's
  report or log before reasoning about rendering, and read `docs/reference/NvidiaGpu.md` and
  `docs/reference/DriverBugs.md` when NVIDIA or driver quirks are in play.
- If the machine matters to the conclusion (perf numbers, render bugs), state the hardware you
  tested or reasoned about in your findings.

## Naming

Lichii is the product name. Anything user-facing or host-visible takes Lichii naming: UI strings, dialogs, window titles, log lines, cubeb/SDL application names, default profile/nickname/device values, docs, dist metadata. Executables are `lichii`, `lichii-cli` and `lichii-room`; the CMake targets behind them keep their historical `yuzu*` names.

Deliberate keeps — do not rename these, and do not "finish" them in passing:

- On-disk `eden` data directories (`LICHII_DIR` keeps the value `"eden"`), `eden_log.txt` (the log filename, `src/common/fs/fs_paths.h` `LOG_FILE`), and the `eden_username`/`eden_token` config keys. Renaming strands every existing user's saves and tokens.
- All `EDEN_*` environment switches. They are the debugging surface documented in `docs/nx2/debug-tools.md` and in the owner's launch scripts.
- Eden web-service URLs and the strings that name them ("Eden Web Services", "valid Eden account").
- SPDX headers (`Eden Emulator Project`), `eglantine.profdata` (release CI), and comments attributing upstream behaviour ("Eden's compiler…", "an Eglantine developer…").
- The stored hotkey key `"Exit Eglantine"` (it displays as "Exit Lichii"; renaming the key orphans saved keybinds).
- The Android app (`org.yuzu.yuzu_emu`) and factual upstream references (PR inventories, session notes).

When merging from `origin/nx2`: nx2 does not carry the rebrand, so keep it through conflicts — take upstream's functional changes, keep the Lichii strings, then sweep the merged files for reintroduced branding. SPDX headers and attribution comments are expected; new user-facing Eden/Eglantine strings are not.

Icons are generated from `dist/lichii.svg` by `tools/update-icons.sh` (needs `magick` and `optipng`). Translation catalogs (`dist/languages/*.ts`) still carry pre-rename source strings; Transifex ownership for Lichii is undecided, so don't hand-edit the catalogs and don't `tx pull -t -f` — it would overwrite local changes with the shared Eden resource.

## Game bug reports

Users report game bugs with Lichii logs (`eden_log.txt`, `eden_log.txt.old`). Each `Booting game: <program id>` line starts a session; the build is on the first line (`Lichii Version: ... | nx2-<sha>`). Known program ids:

| Program id | Game |
|---|---|
| 040074A01BF12000 | Donkey Kong Bananza |
| 040077201BE50000 | Kirby Air Riders |
| 04007C6028AA8000 | Yoshi and the Mysterious Book (demo) |
| 0400D4201C6EA000 | Mario Tennis Fever |
| 0400E88025B1A000 | Star Fox |
| 04007EF01F8AE000 | Fire Emblem: Fortune's Weave |
| 0400C3F00006E000 | Mario Kart World |
| 04008E001F098000 | Nintendo Switch Sports Resort |
| 0400A3A01EE4C000 | Pokémon Pokopia |
| 0400A4802437E000 | Drag x Drive Global Jam |
| 040041E0212AE000 | Fast Fusion |
| 04005B902517C000 | Kirby Air Riders Global Test Ride |
| 040057C02159A000 | Nintendo Switch 2 Welcome Tour |
| 040004F027560000 | Orbitals Global Friends Pass |
| 0400380020056000 | Raiders |
| 0400D1D02399A000 | The Duskbloods network test |
| 0400486024DEC000 | The Legend of Zelda: Ocarina of Time |
| 0400245021D2C000 | Yoshi and the Mysterious Book |

Add ids here when a report identifies a new game. Past investigations for a game live in
`docs/investigations/` — read them first; many symptoms were already traced.

## Before writing a fix

Lichii is a fork of [Eglantine](https://github.com/Djihads80/Eglantine). Do not guess, and do not rewrite a fix upstream already has.

1. `git remote add upstream https://github.com/Djihads80/Eglantine.git && git fetch upstream`.
2. Check `upstream/nx2`, open and closed PRs, unmerged branches, and issues for the game and symptom (`gh pr list -R Djihads80/Eglantine --state all`, `gh issue list -R Djihads80/Eglantine --state all`, `gh pr diff <n>`). Read the diffs, not only the titles.
3. If nx2 is behind `upstream/nx2`, merge upstream first. Port an existing upstream fix before writing a new one, and cite its PR or commit.
4. Write a new fix only when nothing upstream addresses the root cause the log shows.

## Repos and branches

- `origin` is [zortos293/Lichii](https://github.com/zortos293/Lichii): `nx2` lives here and fixes land here through PRs.
- `cyrix` is the owner's fork, [CyrixJD115/Lichii](https://github.com/CyrixJD115/Lichii): fix branches push here for CI.
- `upstream` is [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), the true upstream from "Before writing a fix".
- Work happens on feature branches (`fix/<topic>`); never push to `nx2` on either remote.
- Which repo to push to depends on who owns the work: the owner's branches go to `cyrix`; branches
  intended for the main repo go through PRs against `zortos293/Lichii` `nx2`. If it isn't your
  decision, ask.

## Validation

A change is not done when it compiles in your head — it is done when something ran and the result
is stated. Match validation depth to the change:

- **Docs / comments / markdown**: links resolve, files parse, moved files left no dangling
  references. Grep for references to anything you moved or renamed.
- **Namespace / string changes**: run the rebrand sweep (grep for `eden|eglantine` minus the
  deliberate keeps in [Naming](#naming)) and confirm nothing user-facing changed unintentionally.
- **C++ changes**: compile at least the touched translation units, or rely on CI when no local
  toolchain exists. Never claim a build you didn't run or watch.
- **CI on GitHub Actions**: `build-check` runs automatically on PRs (Linux GCC + Windows clang-cl,
  production build and tests). For a full-matrix validation build (Linux x86_64/aarch64 ±PGO,
  Windows MSVC ±PGO, macOS arm64) dispatch `release-binaries` with `publish_release=false` — this
  validates without publishing. On the main repo, let the owners' required checks decide; on forks,
  dispatch yourself and read the logs — green checkmarks you did not inspect prove nothing.
- **Gameplay behaviour**: CI proves it builds and unit tests pass; it does not prove a game renders
  correctly. Runtime verification needs the owner (or the live debug server) — say plainly what was
  and was not tested.

## Live debug server

Every debug package on the PC launches with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` (in the env argument of `build-fix.sh`; `EDEN_NX2_DEBUG_DIR=<dir>` moves its dumps). Ask the running game from bash on the PC with native Windows Python; MSYS `python` rejects `C:/` paths.

```sh
PY=/c/Users/Zortos/AppData/Local/Programs/Python/Python314/python.exe
$PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"status"}'
$PY -I C:/lx/src/tools/nx2/debug_mcp.py --server 127.0.0.1:6545 --call '{"method":"screenshot"}' --out C:/Users/Zortos/.capy/work/lichii-debug/shot.png
```

Methods: `status`, `frame_times` (`seconds`, `refresh_hz`), `screenshot`, `log_tail` (`after`, `contains`), `log_filter`, `capture_frame` (the frame dump below, taken now; `sampled`, `index`), `capture_image`, `reject_shaders` (`EDEN_NX2_REJECT_HASH` at runtime; `"reject": false` restores). Without `--call` the script is a stdio MCP server. See `docs/nx2/debug-mcp.md`.

- Try a live experiment (screenshot, `reject_shaders` A/B, `capture_frame`, `log_tail`) before asking the owner for a new build or shortcut.
- Use full 16-hex-digit hashes; shorter ones are refused.
- Restore every hash you reject before you finish.
- Never start or stop the emulator. If nothing listens, ask the owner to launch the game.
- Keep sessions light while the owner plays: one request at a time, no capture loops, `sampled` only when you need pass inputs.
- The paths above are the owner's PC layout. On any other machine running the game, the same script
  works with that machine's checkout path and Python.

## RenderDoc and the live server

Use both for graphics investigations. The live server provides emulator state, native shader hashes, logs and reversible shader A/B tests. [RenderDoc](https://github.com/baldurk/renderdoc) inspects the translated Vulkan frame: draw and dispatch state, resource contents, pixel history and shader debugging where supported. Prefer it over extending our dumps when it already exposes the missing evidence.

- Use the live server to identify the failing scene and programs, then capture one affected frame with RenderDoc. Find the first incorrect output and inspect its inputs, bindings, blend/depth state and resource dependencies. Record the build, settings, event IDs and shader hashes so the two views can be correlated.
- The existing integration is `src/core/tools/renderdoc.cpp`, gated by `enable_renderdoc_hotkey`. It requires RenderDoc to be loaded into the process; enabling the hotkey alone does not load it. Ask the owner to launch the capture-enabled package. Never start, stop or inject into their running emulator without their approval.
- [renderdoc-mcp](https://github.com/Linkingooo/renderdoc-mcp) is a third-party replay adapter, not part of RenderDoc. Pin and inspect its version, use a compatible RenderDoc Python module, and verify a real capture opens before relying on it. If an extension, pixel-history query or shader debug operation is unsupported, report that limit rather than treating missing data as a clean result.
- Keep `.rdc` captures on the owner's PC: they can contain game shaders, textures and buffers. Do not upload them to public issues or services. Share only the specific permitted evidence needed for the investigation.
- Capture and replay can alter timing or hide races. Compare with an uncaptured live run before ruling out a synchronization bug. Distinguish measured GPU counters from adapter estimates; use CPU/thread and frame traces for emulator performance.
- A shader replacement that reduces fog or exposure can be a useful visual comparison, but is not proof of an emulator fix. Trace the original pipeline's first incorrect result before shipping a change. Matching shader arithmetic alone does not validate bindings, synchronization or input data.

## Debugging tools

Use these before guessing. The full switch table (frame dumps, shader dumps, `EDEN_NX2_REJECT_HASH`
bisecting, traces, minidumps) lives in `docs/nx2/debug-tools.md` — read it, pick the row that
matches the symptom. The rules that always apply:

- Every switch is off unless its variable is set; pass it as the env argument of `build-fix.sh`.
- Read the tool's output from the log at `*:Info` unless the row says otherwise.
- Turn diagnostics off when the investigation ends; restore every hash you reject.

## How work is split

- One subagent per game, all running in parallel.
- Small research (upstream PR and issue surveys, log reading): a cheap model, DeepSeek V4.1 Flash at max reasoning when its endpoint is available.
- Hard fixes (emulation, shaders, GPU): GPT-6 Astra at high reasoning.
- When a GPT-6 Astra subagent can't find the cause, switch that subagent to Claude Opus 5.5 at high reasoning.
- The main thread (Opus) orchestrates and reviews.

## Builds

Build capacity differs per machine — detect what the machine you're on offers before planning a
build (see [Know the machine you are on](#know-the-machine-you-are-on)):

- **CI (any fork with Actions enabled)**: `build-check` on PRs, `release-binaries` full matrix via
  workflow dispatch. This is the default validation path when no local toolchain exists — see
  [Validation](#validation).
- **Cloud/dev boxes**: build with clang and Ninja for compile and unit-test checks. Configure
  out-of-tree; the repo configures clean with system Qt 6 + Vulkan headers
  (`-DVulkanHeaders_FORCE_BUNDLED=ON` avoids partial-system-install conflicts).
- **The owner's PC** (DESKTOP-4UD89UT, Ryzen 9 5900X): the machine that produces builds the owner
  actually tests. clang-cl with the VS 2022 SDK, Qt 6.8.3 in `C:/lx/qt`, Ninja and sccache. Source
  is `C:/lx/src`, the persistent build dir is `C:/lx/build`, and `C:/lx/configure.sh` holds the
  configure flags. Build and package a fix with one command:
  `bash C:/lx/build-fix.sh <branch> <build-name> "<Game> - <fix>" ["ENV=VAL ..."] ["log filter"]`.
  Every fix gets its own debug build in `C:\Users\Zortos\.capy\work\lichii-debug\<build>\` and a
  desktop shortcut `Lichii Debug - <Game> - <fix>` that launches it with the owner's existing user
  data and keeps the log next to the build, so the owner can run it and send the log back.
- Bundle a round's candidate fixes into one build behind env switches, so the owner tests one package and A/Bs it live through the debug server instead of one build per idea.

## Docs map and maintenance

Docs are the shared memory. Keep them current or the next agent re-derives everything from scratch.

Layout:

- `docs/` root — evergreen contributor guides: `Build.md`, `CI.md`, `Coding.md`, `Debug.md`,
  `Deps.md`, `Options.md`, `Settings.md`, `Caveats.md`, `CrossCompile.md`, `CPMUtil.md`,
  `Development.md`, `compatibility-report.md`.
- `docs/reference/` — standing technical reference: subsystem design (`DesignOverview.md`,
  `HosKernel.md`, `dynarmic/`), GPU/driver notes (`NvidiaGpu.md`, `DriverBugs.md`), video playback
  (`VideoPlayback*.md`), documented behaviours (`native-dlss-replacement.md`, `overwolf-vulkan.md`).
- `docs/investigations/` — dated per-game/per-session debugging records (Bananza, Kirby, Star Fox,
  Duskbloods, shader sweeps…). Read before debugging a game; append new findings to the game's
  existing file or start `<game>-<topic>.md`.
- `docs/nx2/` — the nx2 effort itself: `debug-tools.md` (the authoritative env-switch table),
  `debug-mcp.md`, and nx2 technical notes.
- `docs/user/` — end-user handbook (do not restructure casually; it ships to users).
- `docs/build/`, `docs/performance/`, `docs/img/` — supporting material.

Maintenance rules:

- New research, findings, root causes, A/B results: write them into the matching doc today — not
  "later". Unrecorded findings rot; the next session starts from zero.
- New env switch, renamed tool, changed procedure: update `docs/nx2/debug-tools.md`,
  `docs/nx2/debug-mcp.md` or the guide that describes it, in the same change.
- New game investigation: `docs/investigations/<game>-<topic>.md`, linked from the game's existing
  chain of notes.
- Keep `docs/README.md` (the index) accurate when you add or move docs.
- This file (`AGENTS.md`) is for policy and pointers only. Move knowledge into docs, not here; the
  only reason to edit AGENTS.md is a change in policy, layout, or pointers.

## Toggles and merging

- Experiment switches (env flags that pick between candidate fixes) never merge to nx2. A proven fix ships as unconditional behaviour, and a disproven one is dropped.
- Diagnostic tools (frame dump, `EDEN_NX2_REJECT_HASH`, traces, the debug server) stay in nx2, off unless their variable is set.
