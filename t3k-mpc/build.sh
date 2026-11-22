#!/usr/bin/env bash
# Build TONE3000 (browse and download TONE3000 captures on the MPC; src/t3k.c, ours, with
# vocal-mpc/src/vocal_core.h and the MPC's libcurl).
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
PAGE="Other Things - VST - TONE3000"
INC=(-I"$REPO/vocal-mpc/src" -I"$REPO/nam-mpc/src")
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
mkdir -p "$B"
case "$MODE" in
host)
  gcc -O2 -Wall -Wextra -Wno-unused-function -Wno-format-truncation -fPIC -shared -fvisibility=hidden "${INC[@]}" "$HERE/src/t3k.c" -lm -lpthread -ldl \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/TONE3000-host.so"
  gcc -O2 -Wall -Wextra "$HERE/test/t3k_test.c" -ldl -lm -lpthread -ldl -o "$B/t3k-test-host"
  echo "built $B/TONE3000-host.so" ;;
arm)
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-function "${ARMF[@]}" -fPIC -shared -fvisibility=hidden "${INC[@]}" \
    "$HERE/src/t3k.c" -lm -lpthread -ldl -Wl,--no-undefined -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/TONE3000.so"
  arm-linux-gnueabihf-strip "$B/TONE3000.so"
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/t3k_test.c" -ldl -lm -lpthread -ldl -o "$B/t3k-test-arm"
  echo "built $B/TONE3000.so" ;;
test)
  "$B/t3k-test-host" "$B/TONE3000-host.so"
  qemu-arm -L "$SYSROOT" "$B/t3k-test-arm" "$B/TONE3000.so" ;;
skin)   # the parameter list comes from the plugin's own table
  gcc -O2 -DVC_PARAMS -Wno-unused-function "${INC[@]}" "$HERE/src/t3k.c" -lm -lpthread -ldl -o "$B/gen-params"
  "$B/gen-params" > "$HERE/page/params.json"
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/TONE3000.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/TONE3000.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/TONE3000.so" /usr/lib/nam-mpc/plugins/TONE3000.so \
    > "$B/bundle/entries/TONE3000.xml"
  cat "$B/bundle/entries/TONE3000.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for TONE3000" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
