#!/bin/sh -e

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Cross-builds the MinGW (x86_64, UCRT) FFmpeg package from Linux with llvm-mingw: FFmpeg with
# libdav1d merged into libavcodec.a, plus FFmpeg's native AV1 decoder and the AV1 hardware
# accelerators (Vulkan Video, D3D12VA, D3D11VA, DXVA2, NVDEC), in CPMUtil's CI package layout.
#
# Usage: LLVM_MINGW=<llvm-mingw dir> tools/windows/build-ffmpeg-mingw.sh <work dir>
# Needs: curl, tar with zstd, make, nasm, meson, ninja, pkg-config.
# Output: <work dir>/ffmpeg-mingw-amd64-<VERSION>.tar.zst and its .sha512sum, for a GitHub release
# tagged <VERSION> in the repo cpmfile.json's ffmpeg-ci-av1-mingw entry names.

VERSION=9.0.2-av1hw-mingw-1
FFMPEG_COMMIT=946fcce07b6dcd0331c8cc609192aeff5e1924f8
FFMPEG_PATCH=https://git.eden-emu.dev/eden-emu/eden/raw/commit/ac86f96efadeac4cea0b335cffe3324f95051800/.patch/ffmpeg/0001-steamdeck.patch
DAV1D_VERSION=1.5.4
NVCODEC_VERSION=n13.0.19.0
VULKAN_HEADERS_VERSION=v1.4.345

[ -n "$LLVM_MINGW" ] || { echo "set LLVM_MINGW to the llvm-mingw directory" >&2; exit 1; }
[ -n "$1" ] || { echo "usage: $0 <work dir>" >&2; exit 1; }
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
work=$(cd "$1" && pwd)
src="$work/src"
prefix="$work/prefix"
out="$work/package"
jobs=${JOBS:-4}
host=x86_64-w64-mingw32
PATH="$LLVM_MINGW/bin:$PATH"
export PATH
# Only the packages built here.
PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
PKG_CONFIG_PATH=
export PKG_CONFIG_LIBDIR PKG_CONFIG_PATH

fetch() {
    [ -f "$src/$2" ] || curl -sSfL "$1" -o "$src/$2"
}

mkdir -p "$src" "$prefix/include"
fetch "https://github.com/FFmpeg/FFmpeg/archive/$FFMPEG_COMMIT.tar.gz" ffmpeg.tar.gz
fetch "$FFMPEG_PATCH" ffmpeg.patch
fetch "https://code.videolan.org/videolan/dav1d/-/archive/$DAV1D_VERSION/dav1d-$DAV1D_VERSION.tar.gz" dav1d.tar.gz
fetch "https://github.com/FFmpeg/nv-codec-headers/archive/$NVCODEC_VERSION.tar.gz" nvcodec.tar.gz
fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/$VULKAN_HEADERS_VERSION.tar.gz" vulkan.tar.gz
(cd "$src" && sha256sum ffmpeg.tar.gz ffmpeg.patch dav1d.tar.gz nvcodec.tar.gz vulkan.tar.gz > "$work/sources.sha256")

# Vulkan headers and nv-codec-headers: headers only.
if [ ! -f "$prefix/include/vulkan/vulkan.h" ]; then
    rm -rf "$src/vulkan" && mkdir "$src/vulkan"
    tar -C "$src/vulkan" --strip-components=1 -xzf "$src/vulkan.tar.gz"
    cp -r "$src/vulkan/include/vulkan" "$src/vulkan/include/vk_video" "$prefix/include/"
fi
if [ ! -f "$PKG_CONFIG_LIBDIR/ffnvcodec.pc" ]; then
    rm -rf "$src/nvcodec" && mkdir "$src/nvcodec"
    tar -C "$src/nvcodec" --strip-components=1 -xzf "$src/nvcodec.tar.gz"
    make -C "$src/nvcodec" PREFIX="$prefix" install > "$work/nvcodec.log"
fi

# dav1d, static. Built here with clang, so it needs no libssp (MSYS2's is built with
# -fstack-protector-strong and _FORTIFY_SOURCE).
if [ ! -f "$prefix/lib/libdav1d.a" ]; then
    rm -rf "$src/dav1d" && mkdir "$src/dav1d"
    tar -C "$src/dav1d" --strip-components=1 -xzf "$src/dav1d.tar.gz"
    cat > "$work/meson-cross.txt" <<EOF
[binaries]
c = '$host-clang'
ar = '$host-ar'
strip = '$host-strip'
windres = '$host-windres'
pkg-config = 'pkg-config'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    meson setup "$work/dav1d-build" "$src/dav1d" --cross-file "$work/meson-cross.txt" \
        --prefix="$prefix" --libdir=lib --buildtype=release --default-library=static \
        -Denable_tools=false -Denable_tests=false -Denable_examples=false > "$work/dav1d.log"
    ninja -C "$work/dav1d-build" -j"$jobs" install >> "$work/dav1d.log"
fi

if [ ! -f "$prefix/lib/libavcodec.a" ] || [ -n "$FORCE" ]; then
    rm -rf "$src/ffmpeg" && mkdir "$src/ffmpeg"
    tar -C "$src/ffmpeg" --strip-components=1 -xzf "$src/ffmpeg.tar.gz"
    patch -d "$src/ffmpeg" -Np1 < "$src/ffmpeg.patch"
    hw=""
    for a in d3d11va d3d11va2 d3d12va dxva2 nvdec vulkan; do
        for c in h264 vp9 av1; do hw="$hw --enable-hwaccel=${c}_$a"; done
    done
    hw="$hw --enable-hwaccel=vp8_nvdec"
    # Same flags as the 9.0.2-av1-mingw-1 package (crueter-ci's plus libdav1d), plus the native
    # av1 decoder its hwaccels run in; iconv off (nothing in the package uses it).
    # shellcheck disable=SC2086
    (cd "$src/ffmpeg" && ./configure --prefix="$prefix" --enable-cross-compile \
        --target-os=mingw32 --arch=x86_64 --cross-prefix="$host-" --cc="$host-clang" \
        --cxx="$host-clang++" --pkg-config=pkg-config --pkg-config-flags=--static \
        --extra-cflags="-I$prefix/include" --extra-ldflags="-L$prefix/lib" \
        --disable-avdevice --disable-avformat --disable-doc --disable-everything --disable-ffmpeg \
        --disable-ffprobe --disable-network --enable-static --disable-shared \
        --enable-filter=yadif,scale --enable-small --enable-pic --enable-swresample --disable-iconv \
        --enable-decoder=h264 --enable-decoder=vp8 --enable-decoder=vp9 --enable-decoder=opus \
        --enable-decoder=av1 --enable-libdav1d --enable-decoder=libdav1d \
        --enable-vulkan --enable-cuvid --enable-ffnvcodec --enable-nvdec \
        --enable-d3d11va --enable-d3d12va --enable-dxva2 $hw) > "$work/configure.log" 2>&1 ||
        { tail -40 "$work/configure.log"; tail -60 "$src/ffmpeg/ffbuild/config.log"; exit 1; }
    grep -E "^(External libraries|Enabled decoders|Enabled hwaccels)" -A8 "$work/configure.log" || true
    make -C "$src/ffmpeg" -j"$jobs" > "$work/make.log" 2>&1 || { tail -60 "$work/make.log"; exit 1; }
    make -C "$src/ffmpeg" install > "$work/install.log" 2>&1
fi

rm -rf "$out"
mkdir -p "$out/lib" "$out/LICENSES"
cp -r "$prefix/include" "$out/include"
rm -rf "$out/include/vulkan" "$out/include/vk_video" "$out/include/ffnvcodec" "$out/include/dav1d"
for l in avfilter avutil swresample swscale; do cp "$prefix/lib/lib$l.a" "$out/lib/"; done
# libdav1d merged into libavcodec, so the package links like the CI ones.
printf 'CREATE %s\nADDLIB %s\nADDLIB %s\nSAVE\nEND\n' "$out/lib/libavcodec.a" \
    "$prefix/lib/libavcodec.a" "$prefix/lib/libdav1d.a" | "$host-ar" -M
"$host-ranlib" "$out/lib/libavcodec.a"
cp "$here/ffmpeg-mingw-package.cmake" "$out/CMakeLists.txt"
cp "$0" "$out/"
cp "$work/sources.sha256" "$out/"
cp "$src/ffmpeg/COPYING.LGPLv2.1" "$out/LICENSES/"
cp "$src/dav1d/COPYING" "$out/LICENSES/dav1d-COPYING"
cp "$src/dav1d/doc/PATENTS" "$out/LICENSES/dav1d-PATENTS"
{
    echo "# FFmpeg 9.0.2 + dav1d $DAV1D_VERSION, MinGW (UCRT) amd64, with AV1 hardware decoding"
    echo
    echo "Built by build-ffmpeg-mingw.sh (included) with $("$host-clang" --version | head -1)."
    echo "FFmpeg $FFMPEG_COMMIT with $FFMPEG_PATCH; dav1d $DAV1D_VERSION;"
    echo "nv-codec-headers $NVCODEC_VERSION; Vulkan-Headers $VULKAN_HEADERS_VERSION. Source hashes: sources.sha256."
    echo "License: LGPL 2.1 or later (no --enable-gpl/--enable-nonfree); dav1d BSD-2-Clause."
    echo
    echo '```'
    sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' "$src/ffmpeg/config.h"
    echo '```'
} > "$out/README.md"

echo "== symbols"
for s in ff_libdav1d_decoder ff_av1_decoder ff_av1_vulkan_hwaccel ff_av1_d3d12va_hwaccel \
    ff_av1_d3d11va2_hwaccel ff_av1_nvdec_hwaccel dav1d_open ff_vp9_d3d11va_hwaccel; do
    printf '%s=%s\n' "$s" "$("$host-nm" "$out/lib/libavcodec.a" 2>/dev/null | grep -c " [TDR] $s\$" || true)"
done
archive="$work/ffmpeg-mingw-amd64-$VERSION.tar.zst"
(cd "$out" && tar --zstd -cf "$archive" -- *)
sha512sum "$archive" | cut -d' ' -f1 > "$archive.sha512sum"
echo "package: $archive"
