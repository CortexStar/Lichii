# Overwolf Vulkan startup protection

On Windows, Lichii reads enabled Vulkan layer registrations in both registry
views under HKLM and HKCU before opening the Vulkan loader. Layers identified
by an Overwolf name, manifest location or library path are disabled using their
manifest's `disable_environment` controls and their exact layer names in
`VK_LOADER_LAYERS_DISABLE`. Existing loader exclusions are preserved. This runs
once per process and covers the startup probe and GUI/CLI Vulkan initialization.

No registry entries or installed files are modified. Other applications can
continue using Overwolf. A manifest's disable control can be shared by another
overlay using the same hook implementation; that control also applies within
the Lichii process. This cannot remove hooks injected before Lichii starts.

The workaround follows the launcher comparison that resolved Fortune's Weave
crashes on the testing PC. Crash reports showed heap corruption and loaded
Overwolf Vulkan/OBS hooks, with earlier faults directly in `OWClient.dll`.
It prevents the observed conflict rather than repairing Overwolf itself;
there is no measured performance improvement claimed.

The Windows translation unit was compile-checked. Focused manifest tests cover
vendor/path identification, unrelated OBS isolation, multi-layer manifests,
invalid JSON/types/environment names, empty disable values and repeat discovery.
The integrated startup implementation still needs a direct-launch check on the
affected PC. The working external launcher remains available for comparison.

Loader controls follow the [Khronos layer manifest specification](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderLayerInterface.md).
