#!/usr/bin/env bash
# Build a vocal set of Airwindows plugins (MIT, by Chris Johnson) as VST2 effects for MPC OS.
#   ./build.sh [host|arm|params|skin|bundle]
#     host: x86 builds for testing, arm (default): 32-bit ARM builds for the MPC,
#     params: each plugin's parameter list (params/*.raw.json, from the host builds) and the page sources (pages/),
#     skin: the MPC touchscreen pages (build/skin/, previews build/preview_*.png),
#     bundle: build/bundle/{plugins,skins,entries} for nam-mpc/firmware/patch_image.sh (EXTRA=...)
# Sources: airwindows/<Plugin>/ (copied unchanged from airwindows' plugins/LinuxVST/src). They're written against
# Steinberg's VST2 SDK, which isn't included; shim/ stands in for the part of it they use.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
PLUGINS="DeBess VariMu ButterComp2 Pressure6 Air4 Baxandall3 Tube2 Doublelay Verbity2"
SYSROOT="${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}"

case "$MODE" in
params)
  gcc -O2 -Wall -o "$B/vst_test" "$REPO/tools/vst_test.c" -ldl -lm
  mkdir -p "$HERE/params"
  for p in $PLUGINS; do "$B/vst_test" "$B/host/$p.so" --params-json > "$HERE/params/$p.raw.json"; done
  python3 "$HERE/pages.py" "$HERE/params" "$HERE/pages"
  exit 0 ;;
skin)
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE"/pages/*/
  exit 0 ;;
bundle)
  [ -d "$B/skin" ] && ls "$B"/arm/*.so >/dev/null || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  for p in $PLUGINS; do
    cp "$B/arm/$p.so" "$B/bundle/plugins/Airwindows$p.so"   # prefixed: plugins/ is shared with other bundles
    qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/Airwindows$p.so" \
      "/usr/lib/nam-mpc/plugins/Airwindows$p.so" > "$B/bundle/entries/Airwindows$p.xml"
    skin="airwindows - VST - $p"
    [ -d "$B/skin/$skin" ] || { echo "$p: no page folder named \"$skin\"" >&2; exit 1; }
    cp -a "$B/skin/$skin" "$B/bundle/skins/"
    echo "$p: entry + page \"$skin\""
  done
  exit 0 ;;
host)
  CXX=g++; OUT="$B/host"; FLAGS=(-O2 -g -fPIC); LINKX=() ;;
arm)
  CXX="${CXX:-arm-linux-gnueabihf-g++}"; OUT="$B/arm"
  FLAGS=(-O2 -fPIC -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
  LINKX=(-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL) ;;
*) echo "usage: $0 [host|arm|params|skin|bundle]" >&2; exit 2 ;;
esac
# no -ffast-math: Airwindows' denormal and dither handling relies on exact float behaviour
mkdir -p "$OUT"
pids=()
for p in $PLUGINS; do
  "$CXX" -std=gnu++14 "${FLAGS[@]}" -shared -fvisibility=hidden -D__cdecl= -w -I"$HERE/shim" -I"$REPO/nam-mpc/src" \
    "$HERE"/airwindows/$p/*.cpp "$HERE/shim/audioeffectx.cpp" "${LINKX[@]}" -Wl,--no-undefined -lm \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$OUT/$p.so" &
  pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done   # set -e stops here if any one failed
[ "$MODE" = arm ] && arm-linux-gnueabihf-strip "$OUT"/*.so
ls "$OUT"/*.so | wc -l | xargs echo "built plugins:"
