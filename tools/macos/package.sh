#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

build_dir=${1:?Usage: package.sh BUILD_DIR OUTPUT.zip}
output=${2:?Usage: package.sh BUILD_DIR OUTPUT.zip}
source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT HUP INT TERM

app="$stage/eglantine.app"
ditto "$build_dir/bin/eglantine.app" "$app"
rm -f "$app/Contents/MacOS/nx2_switches.txt"
cp "$build_dir/bin/eden-cli" "$app/Contents/MacOS/eden-cli"
cp "$source_dir/dist/macos/eglantine.entitlements" "$app/Contents/Resources/"
mkdir -p "$app/Contents/Resources/licenses"
cp -R "$source_dir/dist/licenses/." "$app/Contents/Resources/licenses/"
test -s "$app/Contents/Frameworks/libMoltenVK.dylib"

"${QT_ROOT_DIR:?QT_ROOT_DIR must point to the Qt installation}/bin/macdeployqt" "$app" \
    "-executable=$app/Contents/MacOS/eden-cli" -always-overwrite

rm -f "$app/Contents/PlugIns/platforminputcontexts/libqtvirtualkeyboardplugin.dylib"
find "$app/Contents" -type f | while IFS= read -r binary; do
    if ! file -b "$binary" | grep -q 'Mach-O'; then
        continue
    fi
    otool -l "$binary" | awk '/cmd LC_RPATH/ {getline; getline; print $2}' |
        while IFS= read -r rpath; do
            case "$rpath" in
                /opt/homebrew/* | /usr/local/*)
                    install_name_tool -delete_rpath "$rpath" "$binary"
                    ;;
            esac
        done
done
for executable in eglantine eden-cli; do
    binary="$app/Contents/MacOS/$executable"
    if ! otool -l "$binary" | awk '/cmd LC_RPATH/ {getline; getline; print $2}' |
        grep -qx '@executable_path/../Frameworks'; then
        install_name_tool -add_rpath @executable_path/../Frameworks "$binary"
    fi
done

find "$app/Contents/Frameworks" "$app/Contents/PlugIns" -type f | while IFS= read -r binary; do
    if file -b "$binary" | grep -q 'Mach-O'; then
        codesign --force --sign - "$binary"
    fi
done
codesign --force --sign - --entitlements "$app/Contents/Resources/eglantine.entitlements" \
    "$app/Contents/MacOS/eden-cli"
codesign --force --sign - --entitlements "$app/Contents/Resources/eglantine.entitlements" "$app"

ditto -c -k --sequesterRsrc --keepParent "$app" "$output"
ditto -x -k "$output" "$stage/unpacked"
app="$stage/unpacked/eglantine.app"

plutil -lint "$app/Contents/Info.plist"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")" = eglantine
test -s "$app/Contents/Resources/eglantine.icns"
for notice in ffmpeg/COPYING.LGPLv2.1 ffmpeg/LICENSE.md dav1d/COPYING README.md; do
    cmp "$source_dir/dist/licenses/$notice" "$app/Contents/Resources/licenses/$notice"
done
test -s "$app/Contents/Resources/nx2_switches.txt"
test -s "$app/Contents/PlugIns/platforms/libqcocoa.dylib"
codesign --verify --deep --strict --verbose=2 "$app"

find "$app" -type f | while IFS= read -r binary; do
    if ! file -b "$binary" | grep -q 'Mach-O'; then
        continue
    fi
    lipo "$binary" -verify_arch arm64
    otool -l "$binary" | awk '/cmd LC_RPATH/ {getline; getline; print $2}' |
        while IFS= read -r rpath; do
            case "$rpath" in
                /opt/homebrew/* | /usr/local/*)
                    echo "Unbundled search path in $binary: $rpath" >&2
                    exit 1
                    ;;
            esac
        done
    otool -arch arm64 -L "$binary" | tail -n +2 | awk '{print $1}' | while IFS= read -r dependency; do
        case "$dependency" in
            /System/Library/* | /usr/lib/*) ;;
            @rpath/*) test -e "$app/Contents/Frameworks/${dependency#@rpath/}" ;;
            @loader_path/*) test -e "$(dirname "$binary")/${dependency#@loader_path/}" ;;
            @executable_path/*) test -e "$app/Contents/MacOS/${dependency#@executable_path/}" ;;
            *)
                echo "Unbundled dependency in $binary: $dependency" >&2
                exit 1
                ;;
        esac
    done
done

for executable in eglantine eden-cli; do
    codesign --display --entitlements :- "$app/Contents/MacOS/$executable" \
        > "$stage/$executable-entitlements.plist"
    test "$(/usr/libexec/PlistBuddy -c 'Print :com.apple.security.cs.allow-jit' \
        "$stage/$executable-entitlements.plist")" = true
done

python3 - "$app" "$stage" <<'PY'
import ctypes
import os
from pathlib import Path
import subprocess
import sys

app = Path(sys.argv[1])
stage = Path(sys.argv[2])
ctypes.CDLL(str(app / "Contents/Frameworks/libMoltenVK.dylib"))
env = os.environ.copy()
for name in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
    env.pop(name, None)

cli = subprocess.run([str(app / "Contents/MacOS/eden-cli")],
                     capture_output=True, text=True, timeout=30, env=env)
output = cli.stdout + cli.stderr
if cli.returncode != 1 or "No ROM specified" not in output:
    raise SystemExit(f"CLI startup check failed ({cli.returncode}):\n{output}")

with (stage / "gui-startup.log").open("w+") as log:
    gui = subprocess.Popen([str(app / "Contents/MacOS/eglantine")],
                           stdout=log, stderr=subprocess.STDOUT, env=env)
    try:
        gui.wait(timeout=10)
        log.seek(0)
        raise SystemExit(f"GUI exited during startup ({gui.returncode}):\n{log.read()}")
    except subprocess.TimeoutExpired:
        pass
    finally:
        gui.terminate()
        try:
            gui.wait(timeout=5)
        except subprocess.TimeoutExpired:
            gui.kill()
            gui.wait()

print("Packaged GUI, CLI, and MoltenVK startup checks passed")
PY
