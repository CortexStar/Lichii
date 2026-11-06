# Lossless Scaling frame generation

The Vulkan integration supports both standard-quality LSFG 3.1 and its Performance
network. Standard quality is the default. Configure -> Controls -> Frame Generation
contains the global **LSFG Performance Mode** toggle; leave it unchecked for standard
quality. Configure Game -> Graphics can override this choice for an individual game.
Enable frame generation, select Lossless Scaling, and choose a multiplier in Graphics.

Copy `Lossless.dll` from your own Steam installation of Lossless Scaling to
`%APPDATA%\eden\lossless\Lossless.dll` (or `user/lossless/Lossless.dll` for a portable
installation). The DLL must contain the native FP16 SPIR-V resources for both LSFG 3.1
networks. The locally tested Steam DLL reports application version 3.2.2; that number
is separate from the LSFG 3.1 model version. The proprietary DLL is not distributed
with this project. Restart emulation after installing it.

The log reports why LSFG cannot initialize, or confirms that its shader modules and
selected mode loaded. Mode changes rebuild the image graph and reset frame history.
The shader cache is versioned so a cache containing only the old Performance network
is rebuilt automatically.

The host FP16 capability fix is ported from
[Lichii PR #230](https://github.com/zortos293/Lichii/pull/230). It enables the hardware
feature for host shaders while retaining the existing NVIDIA guest-shader workaround.
Both image graphs follow [lsfg-vk 1.0.0](https://github.com/PancakeTAS/lsfg-vk/tree/v1.0.0/framegen).
