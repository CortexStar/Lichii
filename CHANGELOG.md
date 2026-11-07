# Changelog

All notable changes to Eglantine are documented here. Each release's notes are taken from its
`## [X.Y.Z]` section (release candidates use the section of the version they are a candidate for).
New entries go under `## [Unreleased]`; `tools/ci/bump-version.sh` turns them into a version section.

## [Unreleased]

- Ocarina of Time photo mode (F1, or L3 + R3): free camera, frozen world, no HUD, zoom, roll,
  picture ratios, hotsampling the window to the picture's exact size for ReShade, a haze switch,
  and the IGCS camera tools interface for the IGCS Connector add-on's depth of field. See
  `docs/nx2/oot-photo-mode.md`.
- Screenshots on Auto size are taken at the size of the frame the game presents at the internal
  resolution (a Switch 2 game's 1440p frame at 1.5x is 3840x2160, not 2880x1620).

## [0.1.0]

- Initial release candidate
