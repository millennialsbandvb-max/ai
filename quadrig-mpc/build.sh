#!/usr/bin/env bash
# Build Quad Rig (four effect slots + a NAM amp; src/quad_rig.c hosts the engines of Duo FX, Ambient Lab and NAM
# MPC, built here from their own sources with their entry points renamed).
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
PAGE="Other Things - VST - 0Quad Rig"
JOBS="$(nproc 2>/dev/null || echo 4)"
"$REPO/nam-mpc/build.sh" deps >/dev/null   # (unpacks NAM's sources into nam-mpc/build/deps)
DEPS="$REPO/nam-mpc/build/deps"
NAM="$DEPS/NeuralAmpModelerCore-main"
NINC=(-I"$NAM" -I"$NAM/Dependencies/nlohmann" -I"$DEPS/eigen-master" -I"$DEPS/AudioDSPTools-main/dsp" -I"$REPO/nam-mpc/src")
NDEFS=(-DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -DNDEBUG -DEIGEN_MPL2_ONLY)
NAM_SRC=$(cd "$NAM" && ls NAM/*.cpp NAM/*/*.cpp)
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
mkdir -p "$B"

objects() {   # objects <cc> <c++> <obj dir> <flags...>: the engines and Quad Rig
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
  "$cc" "$@" -DVSTPluginMain=duo_entry -DPICKLIST_OFF -I"$REPO/nam-mpc/src" -c "$REPO/duofx-mpc/src/duo_fx.c" -o "$obj/duo_fx.o"
  "$cc" "$@" -Wno-unused-function -DVSTPluginMain=amb_entry -DPICKLIST_OFF -I"$REPO/vocal-mpc/src" -I"$REPO/nam-mpc/src" \
    -c "$REPO/ambient-mpc/src/ambient.c" -o "$obj/ambient.o"
  "$cc" "$@" -Wall -Wextra -I"$REPO/nam-mpc/src" -c "$HERE/src/quad_rig.c" -o "$obj/quad_rig.o"
}
LINK=(-shared -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -Wl,--exclude-libs,ALL -Wl,--no-undefined)

case "$MODE" in
host)
  objects gcc g++ "$B/host-obj" -O2 -fPIC -fvisibility=hidden
  g++ "$B"/host-obj/*.o "${LINK[@]}" -lm -lpthread -o "$B/Quad-Rig-host.so"
  gcc -O2 -Wall -Wextra "$HERE/test/quad_test.c" -ldl -lm -o "$B/quad-test-host"
  echo "built $B/Quad-Rig-host.so" ;;
arm)
  objects arm-linux-gnueabihf-gcc arm-linux-gnueabihf-g++ "$B/arm-obj" -O3 -ffast-math -fPIC -fvisibility=hidden "${ARMF[@]}"
  arm-linux-gnueabihf-g++ -O3 "${ARMF[@]}" "$B"/arm-obj/*.o "${LINK[@]}" -static-libstdc++ -static-libgcc -lm -lpthread -o "$B/Quad-Rig.so"
  arm-linux-gnueabihf-strip "$B/Quad-Rig.so"
  arm-linux-gnueabihf-gcc -O2 "${ARMF[@]}" "$HERE/test/quad_test.c" -ldl -lm -o "$B/quad-test-arm"
  echo "built $B/Quad-Rig.so" ;;
test)
  NAM_MPC_DIR="${NAM_MPC_DIR:-}" "$B/quad-test-host" "$B/Quad-Rig-host.so"
  qemu-arm -L "$SYSROOT" "$B/quad-test-arm" "$B/Quad-Rig.so" ;;
skin)   # the parameter list: ours, the effect names from the engines, and NAM MPC's own entries
  gcc -O2 -fPIC -DQR_PARAMS -I"$REPO/nam-mpc/src" -c "$HERE/src/quad_rig.c" -o "$B/qr_params.o"
  g++ "$B/qr_params.o" "$B/host-obj/duo_fx.o" "$B/host-obj/ambient.o" "$B/host-obj/nam_mpc.o" \
    $(ls "$B"/host-obj/NAM_*.o) -lm -lpthread -ldl -o "$B/gen-params"
  "$B/gen-params" "$REPO/nam-mpc/params.json" > "$HERE/page/params.json"
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/Quad-Rig.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/Quad-Rig.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/Quad-Rig.so" /usr/lib/nam-mpc/plugins/Quad-Rig.so \
    > "$B/bundle/entries/Quad-Rig.xml"
  cat "$B/bundle/entries/Quad-Rig.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for Quad Rig" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
