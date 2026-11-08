#!/usr/bin/env bash
# Build Enz0 X (a tracking guitar synth: Mono, Poly, Arp, Dry; src/enzo.c, our own code, with
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
PAGE="Other Things - VST - Enz0 X"
INC=(-I"$REPO/vocal-mpc/src" -I"$REPO/nam-mpc/src")
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
mkdir -p "$B"
case "$MODE" in
host)
  gcc -O2 -Wall -Wextra -Wno-unused-function -fPIC -shared -fvisibility=hidden "${INC[@]}" "$HERE/src/enzo.c" -lm \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Enz0-X-host.so"
  gcc -O2 -Wall -Wextra "$HERE/test/enzo_test.c" -ldl -lm -o "$B/enzo-test-host"
  gcc -O2 -Wall -Wextra "$HERE/test/track_test.c" -ldl -lm -o "$B/track-test-host"
  echo "built $B/Enz0-X-host.so" ;;
arm)
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-function "${ARMF[@]}" -fPIC -shared -fvisibility=hidden "${INC[@]}" \
    "$HERE/src/enzo.c" -lm -Wl,--no-undefined -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Enz0-X.so"
  arm-linux-gnueabihf-strip "$B/Enz0-X.so"
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/enzo_test.c" -ldl -lm -o "$B/enzo-test-arm"
  echo "built $B/Enz0-X.so" ;;
test)
  "$B/enzo-test-host" "$B/Enz0-X-host.so"
  "$B/track-test-host" "$B/Enz0-X-host.so" 15 3 && HARD=1 "$B/track-test-host" "$B/Enz0-X-host.so" 15 3
  qemu-arm -L "$SYSROOT" "$B/enzo-test-arm" "$B/Enz0-X.so" ;;
skin)   # the parameter list comes from the plugin's own table
  gcc -O2 -DVC_PARAMS -Wno-unused-function "${INC[@]}" "$HERE/src/enzo.c" -lm -o "$B/gen-params"
  "$B/gen-params" > "$HERE/page/params.json"
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/Enz0-X.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/Enz0-X.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/Enz0-X.so" /usr/lib/nam-mpc/plugins/Enz0-X.so \
    > "$B/bundle/entries/Enz0-X.xml"
  cat "$B/bundle/entries/Enz0-X.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for Enz0 X" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
