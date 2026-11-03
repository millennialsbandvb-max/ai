#!/usr/bin/env bash
# Build Ergo EUB (src/ergo.c: the user's HeadRush "Ergo" rigs for their electric upright bass in one insert, hosting a
# NAM MPC engine for the amp, built here from NAM MPC's own sources with its entry point renamed nam_entry).
#   ./build.sh [host|arm|test|skin|bundle]
#     host: x86 build + offline test, arm (default): the plugin for the MPC, test: run the offline test on both
#     (ARM_SYSROOT=<MPC rootfs dump> for the ARM one), skin: its page (build/skin/, previews build/preview_*.png),
#     bundle: build/bundle/{plugins,skins,entries} for nam-mpc/firmware/patch_image.sh (EXTRA=...)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
SYSROOT="${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}"
PAGE="Other Things - VST - Ergo EUB"
JOBS="$(nproc 2>/dev/null || echo 4)"
"$REPO/nam-mpc/build.sh" deps >/dev/null   # (unpacks NAM's sources into nam-mpc/build/deps)
DEPS="$REPO/nam-mpc/build/deps"
NAM="$DEPS/NeuralAmpModelerCore-main"
NINC=(-I"$NAM" -I"$NAM/Dependencies/nlohmann" -I"$DEPS/eigen-master" -I"$DEPS/AudioDSPTools-main/dsp" -I"$REPO/nam-mpc/src")
NDEFS=(-DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -DNDEBUG -DEIGEN_MPL2_ONLY)
NAM_SRC=$(cd "$NAM" && ls NAM/*.cpp NAM/*/*.cpp)
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
CINC=(-I"$REPO/nam-mpc/src" -I"$REPO/vocal-mpc/src")
mkdir -p "$B"

objects() {   # objects <cc> <c++> <obj dir> <flags...>: NAM MPC (as an engine) and Ergo EUB
  local cc="$1" cxx="$2" obj="$3" f o pids=()
  shift 3
  mkdir -p "$obj"
  for f in $NAM_SRC; do
    o="$obj/$(echo "$f" | tr / _).o"
    [ "$o" -nt "$NAM/$f" ] && continue
    "$cxx" -std=c++20 "$@" -fvisibility-inlines-hidden "${NINC[@]}" "${NDEFS[@]}" -c "$NAM/$f" -o "$o" &
    pids+=($!)
    if [ ${#pids[@]} -ge "$JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
  done
  for f in "${pids[@]}"; do wait "$f"; done
  "$cxx" -std=c++20 "$@" "${NINC[@]}" "${NDEFS[@]}" -DVSTPluginMain=nam_entry -DPICKLIST_OFF -c "$REPO/nam-mpc/src/nam_mpc.cpp" -o "$obj/nam_mpc.o"
  "$cc" "$@" -Wall -Wextra -Wno-unused-function "${CINC[@]}" -c "$HERE/src/ergo.c" -o "$obj/ergo.o"
}
LINK=(-shared -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -Wl,--exclude-libs,ALL -Wl,--no-undefined)

case "$MODE" in
host)
  objects gcc g++ "$B/host-obj" -O2 -fPIC -fvisibility=hidden
  g++ "$B"/host-obj/*.o "${LINK[@]}" -lm -lpthread -o "$B/Ergo-EUB-host.so"
  gcc -O2 -Wall -Wextra "$HERE/test/ergo_test.c" -ldl -lm -o "$B/ergo-test-host"
  echo "built $B/Ergo-EUB-host.so" ;;
arm)
  objects arm-linux-gnueabihf-gcc arm-linux-gnueabihf-g++ "$B/arm-obj" -O3 -ffast-math -fPIC -fvisibility=hidden "${ARMF[@]}"
  arm-linux-gnueabihf-g++ -O3 "${ARMF[@]}" "$B"/arm-obj/*.o "${LINK[@]}" -static-libstdc++ -static-libgcc -lm -lpthread -o "$B/Ergo-EUB.so"
  arm-linux-gnueabihf-strip "$B/Ergo-EUB.so"
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/ergo_test.c" -ldl -lm -o "$B/ergo-test-arm"
  echo "built $B/Ergo-EUB.so" ;;
test)
  "$B/ergo-test-host" "$B/Ergo-EUB-host.so"
  qemu-arm -L "$SYSROOT" "$B/ergo-test-arm" "$B/Ergo-EUB.so" ;;
skin)   # the parameter list: ours, then NAM MPC's own entries as amp_*
  gcc -O2 -DERGO_PARAMS -Wno-unused-function "${CINC[@]}" "$HERE/src/ergo.c" -lm -o "$B/gen-params" -Wl,--unresolved-symbols=ignore-all
  "$B/gen-params" "$REPO/nam-mpc/params.json" > "$HERE/page/params.json"
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/Ergo-EUB.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/Ergo-EUB.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/Ergo-EUB.so" /usr/lib/nam-mpc/plugins/Ergo-EUB.so \
    > "$B/bundle/entries/Ergo-EUB.xml"
  cat "$B/bundle/entries/Ergo-EUB.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for Ergo EUB" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
