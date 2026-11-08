# GameInput header

Microsoft GameInput's public header, used by `src/input_common/drivers/gameinput_source.cpp` to
read gamepads through GameInput's reading callbacks on Windows.

- Source: NuGet package `Microsoft.GameInput` 4.0.306
  (https://www.nuget.org/packages/Microsoft.GameInput/4.0.306), file
  `native/include/v3/GameInput.h` (API version 3). SHA-256 of the file:
  `fbb769bef01b133dbb62e3622a7137482cb52b642ea21de080cb98279ef9610f`.
- Licence: MIT, stated at the top of the header ("This file is licensed under the MIT License";
  the package's README: "Updated public header and static library (lib) license to MIT").

The file is unmodified. Only the header is used: Lichii links no GameInput library and ships no
GameInput binary. The GameInput runtime is Microsoft's redistributable (its own licence terms),
installed separately; Lichii loads `GameInputRedist.dll` from System32 at run time and reads
gamepads through SDL when it is not installed.
