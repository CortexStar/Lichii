# Lichii developer documentation

Are you just a casual user? Take a look at our [User Handbook](./user) then!

To contribute to this fork, see [CONTRIBUTING.md](../CONTRIBUTING.md). AI agents: start with
[`AGENTS.md`](../AGENTS.md) — it routes to the rest of this tree and sets the working policy.

## Getting started

- **[General Build Instructions](./Build.md)**
- **[Dependencies](./Deps.md)**
- **[CMake Options](./Options.md)**
- **[Cross Compiling](./CrossCompile.md)**
- **[Platform-Specific Caveats](./Caveats.md)**
- **[Building Older Commits](./build/OlderCommits.md)** — also [Android](./build/Android.md)

## Contributing

- **[Coding guidelines](./Coding.md)**
- **[Development Guidelines](./Development.md)**
- **[CI and release guide](./CI.md)**
- **[Compatibility reports](./compatibility-report.md)**
- **[Debug Guidelines](./Debug.md)**
  - **[Debug switches](./nx2/debug-tools.md)** — the authoritative env-switch table
  - **[Debug server / MCP](./nx2/debug-mcp.md)**
  - **[RenderDoc](./Debug.md#renderdoc)**
- **[CPM - CMake Package Manager](./CPMUtil.md)**

## Reference

Standing technical documentation about subsystems, hardware and behaviours:

- Subsystems: **[Design Overview](./reference/DesignOverview.md)**,
  **[HOS Kernel](./reference/HosKernel.md)**, **[Dynarmic](./dynarmic/README.md)**
- GPU and drivers: **[The NVIDIA SM86 (Maxwell) GPU](./reference/NvidiaGpu.md)**,
  **[Driver Bugs](./reference/DriverBugs.md)**
- Video playback: **[Debug](./reference/VideoPlaybackDebug.md)**,
  **[Upstream audit](./reference/VideoPlaybackUpstreamAudit.md)**,
  **[Upstream PR inventory](./reference/VideoPlaybackUpstreamInventory.md)**
- Documented behaviours: **[Game-DLSS replacement](./reference/native-dlss-replacement.md)**,
  **[Overwolf Vulkan startup protection](./reference/overwolf-vulkan.md)**
- Settings: **[Settings](./Settings.md)**
- nx2 effort: **[debug tools](./nx2/debug-tools.md)**, **[debug MCP](./nx2/debug-mcp.md)**,
  **[async native pipelines](./nx2/async-native-pipelines.md)**,
  **[microphone](./nx2/microphone.md)**,
  **[Ocarina dynamic fps](./nx2/ocarina-dynamic-fps.md)**
- Performance: **[MKW integration](./performance/MKW-INTEGRATION.md)**

## Investigations

Dated, per-game debugging records live in [`docs/investigations/`](./investigations) — Bananza,
Kirby Air Riders, Star Fox, Duskbloods, and the shader-sweep campaigns. Read the game's existing
chain before debugging it, and append new findings there.

## User docs

The end-user handbook is [docs/user](./user) (QuickStart, Controllers, Graphics, Mods,
Troubleshoot, …).

## Externals

Other useful resources in general, take a quick read if you need.

- **[SwitchBrew](https://switchbrew.org/wiki/Main_Page)**
- **[IPS file format](https://zerosoft.zophar.net/ips.php)**
- **[IPSwitch file format](https://github.com/3096/ipswitch)**
