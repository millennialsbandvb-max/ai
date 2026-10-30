#!/usr/bin/env bash
# Build the EQ plugins (our own code, src/): GQ31 Graphic (a 31-band graphic EQ, rack-graphic style) and X-Para EQ
# (a console-style 4-band parametric), under the maker "Other Things"; they use vocal-mpc/src/vocal_core.h.
#   ./build.sh [host|arm|test|skin|bundle]
#     host: x86 builds + offline test, arm (default): the plugins for the MPC, test: run the offline test on both
#     (ARM_SYSROOT=<MPC rootfs dump> for the ARM ones), skin: their pages (build/skin/, previews build/preview_*.png),
#     bundle: build/bundle/{plugins,skins,entries} for nam-mpc/firmware/patch_image.sh (EXTRA=...)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
SYSROOT="${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}"
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
# source : plugin file : page folder
PLUGINS=("graphic_eq:GQ31-Graphic:gq31" "para_eq:X-Para-EQ:xpara")
mkdir -p "$B"
case "$MODE" in
host)
  for p in "${PLUGINS[@]}"; do IFS=: read -r src so _ <<<"$p"
    gcc -O2 -Wall -Wextra -Wno-unused-function -fPIC -shared -fvisibility=hidden -I"$REPO/nam-mpc/src" -I"$REPO/vocal-mpc/src" "$HERE/src/$src.c" -lm \
      -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/$so-host.so"
  done
  gcc -O2 -Wall -Wextra "$HERE/test/eq_test.c" -ldl -lm -o "$B/eq-test-host"
  echo "built host plugins" ;;
arm)
  for p in "${PLUGINS[@]}"; do IFS=: read -r src so _ <<<"$p"
    arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -Wno-unused-function "${ARMF[@]}" -fPIC -shared -fvisibility=hidden -I"$REPO/nam-mpc/src" -I"$REPO/vocal-mpc/src" \
      "$HERE/src/$src.c" -lm -Wl,--no-undefined -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/$so.so"
    arm-linux-gnueabihf-strip "$B/$so.so"
  done
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/eq_test.c" -ldl -lm -o "$B/eq-test-arm"
  echo "built ARM plugins" ;;
test)
  "$B/eq-test-host" "$B/GQ31-Graphic-host.so" "$B/X-Para-EQ-host.so"
  qemu-arm -L "$SYSROOT" "$B/eq-test-arm" "$B/GQ31-Graphic.so" "$B/X-Para-EQ.so" ;;
skin)   # each page's parameter list comes from its plugin's own table
  dirs=()
  for p in "${PLUGINS[@]}"; do IFS=: read -r src _ page <<<"$p"
    gcc -O2 -DVC_PARAMS -Wno-unused-function -I"$REPO/nam-mpc/src" -I"$REPO/vocal-mpc/src" "$HERE/src/$src.c" -lm -o "$B/gen-params-$page"
    "$B/gen-params-$page" > "$HERE/page/$page/params.json"
    dirs+=("$HERE/page/$page/")
  done
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "${dirs[@]}" ;;
bundle)
  [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  for p in "${PLUGINS[@]}"; do IFS=: read -r _ so _ <<<"$p"
    cp "$B/$so.so" "$B/bundle/plugins/"
    qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/$so.so" "/usr/lib/nam-mpc/plugins/$so.so" > "$B/bundle/entries/$so.xml"
    cat "$B/bundle/entries/$so.xml"
  done
  for n in "31 Smashly" "X-Para EQ"; do
    [ -d "$B/bundle/skins/Other Things - VST - $n" ] || { echo "no page folder for $n" >&2; exit 1; }
  done ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
