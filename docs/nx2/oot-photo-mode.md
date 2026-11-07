# Ocarina of Time photo mode

A free camera for The Legend of Zelda: Ocarina of Time (title `0400486024DEC000`, module `main`,
build ID `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`), in the spirit of Frans Bouma's camera tools
(UUU): the world freezes, the HUD goes, and the camera moves freely. Pictures can be taken at any of
nine ratios at the game's full internal resolution, the window can be hotsampled to the picture's
exact size for tools that capture it (ReShade), and Lichii acts as the IGCS camera tools for the
IGCS Connector add-on, so IGCS depth of field works.

It needs the `Photo Mode (built-in)` Add-On, which is on by default; turning it off in Add-Ons
turns photo mode off. Nothing changes until photo mode is switched on.

## Controls

Toggle: **F1** (hotkey "Toggle Photo Mode"), or **L3 + R3** on the controller.

| Input | Action |
| --- | --- |
| Left stick | Move along where the camera looks |
| Right stick | Look around |
| ZR / ZL | Up / down |
| R (hold) / L (hold) | Four times faster / slower, finer look and zoom |
| D-pad up / down | Zoom in / out (vertical field of view 5 to 120 degrees; the game uses 50) |
| D-pad left / right | Roll |
| L3, or Ctrl+F1 | Picture ratio: 16:9 (the whole frame), 21:9, 32:9, 3:2, 4:3, 1:1, 4:5, 2:3, 9:16 |
| Minus, or Shift+F1 | Hotsampling on / off |
| R3 | Haze off / on |
| X | HUD on / off |
| A | Unfreeze / freeze the world |
| Plus | Picture (saved to the screenshots folder, no Save As dialog) |
| Y | Back to the starting camera and field of view |
| B, F1 or L3 + R3 | Leave photo mode |

While photo mode is on the game gets no controller input (the controller is in configuration
mode, as while it is being remapped).

## Pictures and hotsampling

Screenshots with the screenshot size on Auto are now taken at the size of the frame the game
presents, at the internal resolution: a Switch 2 game's 1440p frame at 1.5x is 3840x2160 (it was
1080 x 1.5 = 2880x1620 before). In photo mode the picture is the centred crop at the chosen ratio,
pixel for pixel. A frame on screen (a translucent Qt overlay) shows what the picture keeps.

Hotsampling makes the window frameless at the picture's exact size, which can be larger than the
screen, and the presenter shows only the crop, so ReShade's screenshot gets the whole picture as
it does with UUU. At 1.5x a 4:5 picture is 1728x2160; at 3x it is 3456x4320.

## IGCS depth of field

`lichii.exe` exports `IGCS_StartScreenshotSession`, `IGCS_MoveCameraMultishot`,
`IGCS_MoveCameraPanorama` and `IGCS_EndScreenshotSession`, the functions the IGCS Connector add-on
for ReShade (<https://github.com/FransBouma/IgcsConnector>) looks for. When photo mode turns on it
calls the add-on's `connectFromCameraTools()` (ReShade then shows "Camera tools connected") and
writes the add-on's `CameraToolsData` every frame. During a session the camera moves from where the
session started (left/right and up/down in world units, about a metre each) and comes back when it
ends; the sticks are locked meanwhile. Set "frames to wait" to 2 to 4.

## How it works

- **Freeze**: the game's time manager (`**(main+0x4c7e890)`) multiplies every frame delta by a
  global scale at `+0x1c0`; photo mode sets it to 1e-4 (not 0: the game takes the reciprocal).
- **Camera**: the camera function gets the active camera object from `main+0x2bcedac`. The
  built-in patch branches from `main+0x2bcedc0` to a 116-byte cave in the executable padding at
  `main+0x3C46420` that records the object and, while enabled, copies the emulator's camera over it
  from BSS state at `main+0x4E9FE40` (0x90 bytes, no code references it): the world-to-view matrix
  (`+0x8`), eye, target and up (`+0x38`), and with a zoom the field of view block (`+0x130`) and
  the projections' scale (`+0x9c`/`+0xb0` and the copy at `+0xdc`/`+0xf0`).
- **HUD**: the HUD is drawn into the anti-aliasing output image without depth and with blending.
  The game also draws characters and foliage into that image after its anti-aliasing, with the
  scene's depth, so only the depth-less blended draws are skipped. The output image is taken from
  the output pass (`6bd913e1f34e83eb`, or the per-ratio passes when the game renders below its
  output size).
- **Haze**: compute pass `780c0536b89fc89e` lays the haze over the scene; R3 drops it.

## Known limits

- Link's hair and cap are a spring simulation on a clock of its own: after the freeze they settle
  over about 30 seconds and then drift very slowly. For IGCS depth of field, wait about 30 seconds
  after entering photo mode. Slowing the game's raw frame step freezes them but crashes the game
  when time resumes.
- No depth of field pass was found in gameplay (all 102 compute programs were dropped in turn).
- Offsets belong to the one build ID above; other builds skip the Add-On.
