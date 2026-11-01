# FFmpeg and dav1d notices

These notices accompany Eglantine distributions that include FFmpeg or dav1d.
They supplement the project's other license materials. A notice's presence does
not mean that every build includes that library or uses the version listed here.

## Audited AV1 dependency package

The Windows MSVC x64 and bundled Linux x64 AV1 dependency is
[Takoroka/ffmpeg-av1 9.0.2-av1-1](https://github.com/Takoroka/ffmpeg-av1/releases/tag/9.0.2-av1-1).
The examined libraries report FFmpeg 9.0.2 and dav1d 1.5.4. The package identifies
these public sources:

- FFmpeg commit `946fcce07b6dcd0331c8cc609192aeff5e1924f8`:
  https://github.com/FFmpeg/FFmpeg/tree/946fcce07b6dcd0331c8cc609192aeff5e1924f8
- dav1d 1.5.4, commit `54706fc6bc0cdecab7e9593974a4039cc038fca7`:
  https://code.videolan.org/videolan/dav1d/-/tree/54706fc6bc0cdecab7e9593974a4039cc038fca7

`ffmpeg/COPYING.LGPLv2.1` and `ffmpeg/LICENSE.md` reproduce the FFmpeg license
text and licensing overview from that source revision. The examined FFmpeg
libraries report LGPL version 2.1 or later. `dav1d/COPYING` reproduces the
dav1d BSD-2-Clause notice, including the VideoLAN and dav1d authors' copyright.
The dependency package incorporates dav1d code into avcodec.

Other platforms and system-library builds can use different library versions
and configurations. FFmpeg's applicable license also depends on its enabled
components, as described in `ffmpeg/LICENSE.md`.

## Distribution requirements

These files provide notices, not a complete compliance assessment or a written
source offer. The public source references do not establish that the dependency
binaries are reproducible from those revisions. Before distributing binaries,
verify the applicable licenses and provide the corresponding library source,
modifications, build material, and relinking or replacement mechanism required
by the chosen LGPL compliance method. Static linkage needs particular review.
Check notices and obligations for other bundled dependencies as well.
