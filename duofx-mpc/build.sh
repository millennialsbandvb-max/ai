#!/usr/bin/env bash
# Build Duo FX (an H90-style dual effect: two algorithm slots, series or parallel; src/duo_fx.c, our own code).
#   ./build.sh [host|arm|test|skin|bundle]
#     host: x86 build + offline test, arm (default): the plugin for the MPC, test: run the offline test on both
#     (ARM_SYSROOT=<MPC rootfs dump> for the ARM one), skin: its page (build/skin/, previews build/preview_page_*.png),
#     bundle: build/bundle/{plugins,skins,entries} for nam-mpc/firmware/patch_image.sh (EXTRA=...)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
SYSROOT="${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}"
PAGE="Other Things - VST - Duo FX"
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
mkdir -p "$B"
case "$MODE" in
host)
  gcc -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden -I"$REPO/nam-mpc/src" "$HERE/src/duo_fx.c" -lm \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Duo-FX-host.so"
  gcc -O2 -Wall -Wextra "$HERE/test/duo_test.c" -ldl -lm -o "$B/duo-test-host"
  echo "built $B/Duo-FX-host.so" ;;
arm)
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra "${ARMF[@]}" -fPIC -shared -fvisibility=hidden -I"$REPO/nam-mpc/src" \
    "$HERE/src/duo_fx.c" -lm -Wl,--no-undefined -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/Duo-FX.so"
  arm-linux-gnueabihf-strip "$B/Duo-FX.so"
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/duo_test.c" -ldl -lm -o "$B/duo-test-arm"
  echo "built $B/Duo-FX.so" ;;
test)
  "$B/duo-test-host" "$B/Duo-FX-host.so"
  qemu-arm -L "$SYSROOT" "$B/duo-test-arm" "$B/Duo-FX.so" ;;
skin)   # the parameter list comes from the plugin's own tables, so the page can't drift from it
  gcc -O2 -DDUO_FX_PARAMS -I"$REPO/nam-mpc/src" "$HERE/src/duo_fx.c" -lm -o "$B/gen-params"
  "$B/gen-params" > "$HERE/page/params.json"
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/Duo-FX.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/Duo-FX.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/Duo-FX.so" /usr/lib/nam-mpc/plugins/Duo-FX.so \
    > "$B/bundle/entries/Duo-FX.xml"
  cat "$B/bundle/entries/Duo-FX.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for Duo FX" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
