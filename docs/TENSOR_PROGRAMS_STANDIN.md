# Tensor programs: emulate or stand in

Switch 2 games run neural networks as Ampere `HMMA` (tensor) instructions: Ocarina of Time's
movie enhancement and gameplay upscaler (11 programs, the largest has 768 `HMMA`s), Fire
Emblem's and Star Fox's DLSS networks and others. Lichii lowers `HMMA` to Vulkan cooperative
matrices where the device has `VK_KHR_cooperative_matrix` (and the start-up self-test passes), and
to a software expansion everywhere else. The expansion compiles for minutes and runs at a few FPS
(MoltenVK on Macs, Mesa and Windows Intel drivers, older GPUs).

## Routing

`KeplerCompute::EmulatesNativeTensorPrograms()` decides per compute environment:

1. Donkey Kong Bananza (`040074A01BF12000`) is always emulated; rejected, its programs write
   nothing and the screen goes purple.
2. Setting `tensor_programs` (Graphics > Advanced, per game): `Native` emulates, `Standin` does
   not, `Auto` asks the rasterizer.
3. Vulkan answers `PipelineCache::SupportsCooperativeHmma()` (`Profile::support_cooperative_hmma`).

When not emulated, `LoweringOptions::reject_matrix` makes the lowering reject `HMMA`. The
dispatch loop treats the `matrix multiply` rejection as before and calls
`KeplerCompute::UpscaleInsteadOfNativeProgram()`, which finds the network's input and output
images in constant buffer 0 and blits one to the other (see `FE_UPSCALE_STANDIN.md`,
`STARFOX_UPSCALE_STANDIN.md`). Ocarina's gameplay network is paired by its packed
`RGB9E5` input (cbuf0+0xc0, squared back on copy) and `B10G11R11` output (+0xd0).

The decision is logged once at renderer start (`Tensor programs: ...`). A changed setting needs a
game restart.
