#!/usr/bin/env bash
# Usage: build_pgo.sh <build-dir> <extra C/CXX flags...>
set -euo pipefail
cd "$(dirname "$0")"
bdir="$1"; shift
flags="$*"
cmake -S . -B "$bdir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DYUZU_TESTS=OFF -DYUZU_CMD=ON \
  -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF \
  -DYUZU_USE_BUNDLED_QT=OFF -DYUZU_USE_BUNDLED_SDL3=ON -DYUZU_USE_BUNDLED_FFMPEG=ON \
  -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON \
  -DENABLE_UPDATE_CHECKER=OFF -DLICHII_BUILD_TAG=build-88 -DLICHII_BUILD_PLATFORM=windows-x86_64-pgo \
  -DUSE_DISCORD_PRESENCE=OFF -DUSE_CCACHE=ON \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags"
cmake --build "$bdir" --target yuzu yuzu-cmd --parallel 16
