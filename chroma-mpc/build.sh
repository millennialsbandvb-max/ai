#!/usr/bin/env bash
# Build Chr0ma Console (four effect modules in series: Character, Movement, Diffusion, Texture; src/chroma.c, ours, with
# vocal-mpc/src/vocal_core.h's building blocks).
#   ./build.sh [host|arm|test|skin|bundle]
#     host: x86 build + offline test, arm (default): the plugin for the MPC, test: run the offline test on both
#     (ARM_SYSROOT=<MPC rootfs dump> for the ARM one), skin: its page (build/skin/, preview build/preview_page_0.png),
#     bundle: build/bundle/{plugins,skins,entries} for nam-mpc/firmware/patch_image.sh (EXTRA=...)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
SYSROOT="${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}"
PAGE="Other Things - VST - Chr0ma Console"
INC=(-I"$REPO/vocal-mpc/src" -I"$REPO/nam-mpc/src")
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
mkdir -p "$B"
case "$MODE" in
host)
  gcc -O2 -Wall -Wextra -Wno-unused-function -fPIC -shared -fvisibility=hidden "${INC[@]}" "$HERE/src/chroma.c" -lm \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Chr0ma-Console-host.so"
  gcc -O2 -Wall -Wextra "$HERE/test/chroma_test.c" -ldl -lm -o "$B/chroma-test-host"
  echo "built $B/Chr0ma-Console-host.so" ;;
arm)
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-function "${ARMF[@]}" -fPIC -shared -fvisibility=hidden "${INC[@]}" \
    "$HERE/src/chroma.c" -lm -Wl,--no-undefined -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Chr0ma-Console.so"
  arm-linux-gnueabihf-strip "$B/Chr0ma-Console.so"
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/chroma_test.c" -ldl -lm -o "$B/chroma-test-arm"
  echo "built $B/Chr0ma-Console.so" ;;
test)
  "$B/chroma-test-host" "$B/Chr0ma-Console-host.so"
  qemu-arm -L "$SYSROOT" "$B/chroma-test-arm" "$B/Chr0ma-Console.so" ;;
skin)   # the parameter list comes from the plugin's own table
  gcc -O2 -DVC_PARAMS -Wno-unused-function "${INC[@]}" "$HERE/src/chroma.c" -lm -o "$B/gen-params"
  "$B/gen-params" > "$HERE/page/params.json"
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/Chr0ma-Console.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/Chr0ma-Console.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/Chr0ma-Console.so" /usr/lib/nam-mpc/plugins/Chr0ma-Console.so \
    > "$B/bundle/entries/Chr0ma-Console.xml"
  cat "$B/bundle/entries/Chr0ma-Console.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for Chr0ma Console" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
