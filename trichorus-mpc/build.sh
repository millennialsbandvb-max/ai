#!/usr/bin/env bash
# Build Tri Chorus (a three-voice bucket-brigade chorus with vibrato and shimmer; src/tri_chorus.c, our own code).
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
PAGE="Other Things - VST - Tri Chorus"
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
mkdir -p "$B"
case "$MODE" in
host)
  gcc -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden -I"$REPO/nam-mpc/src" "$HERE/src/tri_chorus.c" -lm \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Tri-Chorus-host.so"
  gcc -O2 -Wall -Wextra "$HERE/test/tri_test.c" -ldl -lm -o "$B/tri-test-host"
  echo "built $B/Tri-Chorus-host.so" ;;
arm)
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra "${ARMF[@]}" -fPIC -shared -fvisibility=hidden -I"$REPO/nam-mpc/src" \
    "$HERE/src/tri_chorus.c" -lm -Wl,--no-undefined -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Tri-Chorus.so"
  arm-linux-gnueabihf-strip "$B/Tri-Chorus.so"
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/tri_test.c" -ldl -lm -o "$B/tri-test-arm"
  echo "built $B/Tri-Chorus.so" ;;
test)
  "$B/tri-test-host" "$B/Tri-Chorus-host.so"
  qemu-arm -L "$SYSROOT" "$B/tri-test-arm" "$B/Tri-Chorus.so" ;;
skin)   # the parameter list comes from the plugin's own table
  gcc -O2 -DTRI_CHORUS_PARAMS -I"$REPO/nam-mpc/src" "$HERE/src/tri_chorus.c" -lm -o "$B/gen-params"
  "$B/gen-params" > "$HERE/page/params.json"
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/Tri-Chorus.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/Tri-Chorus.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/Tri-Chorus.so" /usr/lib/nam-mpc/plugins/Tri-Chorus.so \
    > "$B/bundle/entries/Tri-Chorus.xml"
  cat "$B/bundle/entries/Tri-Chorus.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for Tri Chorus" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
