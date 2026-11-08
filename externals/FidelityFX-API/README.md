# AMD FSR SDK API headers (subset)

The C API headers of AMD's FSR SDK, used to call AMD FSR 4 upscaling and AMD FSR frame
generation through AMD's signed runtime DLLs (`src/video_core/vulkan_common/amd_ffx.cpp`).

- Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag `v2.4.0`
  (commit `7539ae8b6568ad29ed78a753404351daf3b5cca7`), `Kits/FidelityFX/`.
- Licence: MIT, `LICENSE.txt` (the notice at the top of each header).
- Files, unmodified, at their SDK paths so the headers' relative includes resolve:
  `api/include/ffx_api.h`, `api/include/ffx_api_types.h`, `api/include/vk/ffx_api_vk.h`,
  `upscalers/include/ffx_upscale.h`, `framegeneration/include/ffx_framegeneration.h`,
  `framegeneration/include/ffx_framegeneration_api_types.h`,
  `framegeneration/include/vk/ffx_api_framegeneration_vk.h`.

No AMD binary is part of Lichii. The signed DLLs (`amd_fidelityfx_loader_vk.dll`,
`amd_fidelityfx_upscaler_vk.dll`, `amd_fidelityfx_framegeneration_vk.dll`) are closed source
under AMD's licence (no reverse engineering) and are loaded at run time from a folder the user
chooses.
