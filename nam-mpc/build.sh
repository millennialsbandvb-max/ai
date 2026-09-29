#!/usr/bin/env bash
# Build NAM MPC.
#   ./build.sh            all of: arm, skin, package
#   ./build.sh arm        the plugin for the MPC (32-bit ARM) + the on-device test tool (build/NAM-MPC.so, build/nam-test)
#   ./build.sh skin       the MPC touchscreen page (build/skin/) and preview images (build/preview_*.png)
#   ./build.sh package    the release zip (build/NAM-MPC-<version>.zip)
#   ./build.sh deps       only unpack the source zips into build/deps
#   ./build.sh host       an x86 build of the plugin and test tool (with ASan) and run the offline test
# ARM compiler: arm-linux-gnueabihf-g++ (apt install g++-arm-linux-gnueabihf), or CXX=g++ inside Docker's
# arm32v7/gcc:12 image (glibc 2.36; what CI uses, see .github/workflows/build-nam.yml).
# Sources come from the zips at the repo root: NeuralAmpModelerCore, AudioDSPTools (resampler), Eigen, and
# mpc-vst-plugins (skin tools). The skin needs Python 3 with Pillow.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-all}"
B="$HERE/build"
DEPS="$B/deps"
VERSION="$(sed -n 's/^#define PLUG_VERSION \([0-9]*\).*/\1/p' "$HERE/src/nam_mpc.cpp")"
VERSION_STR="$((VERSION / 1000)).$(((VERSION / 100) % 10)).$(((VERSION / 10) % 10))"
JOBS="$(nproc 2>/dev/null || echo 4)"

mkdir -p "$DEPS"
for z in NeuralAmpModelerCore-main AudioDSPTools-main eigen-master mpc-vst-plugins-main; do
  [ -d "$DEPS/$z" ] || unzip -q "$REPO/$z.zip" -d "$DEPS"
done
NAM="$DEPS/NeuralAmpModelerCore-main"
INC=(-I"$NAM" -I"$NAM/Dependencies/nlohmann" -I"$DEPS/eigen-master" -I"$DEPS/AudioDSPTools-main/dsp")
DEFS=(-DNAM_SAMPLE_FLOAT -DNAM_ENABLE_A2_FAST -DNDEBUG -DEIGEN_MPL2_ONLY)
NAM_SRC=$(cd "$NAM" && ls NAM/*.cpp NAM/*/*.cpp)
[ "$MODE" = deps ] && exit 0   # just unpack the sources (CI does this before entering the ARM container)

# compile <cxx> <objdir> <flags...>: NAM's sources in parallel, then the plugin
compile() {
  local cxx="$1" obj="$2" f o pids=()
  shift 2
  mkdir -p "$obj"
  for f in $NAM_SRC; do
    o="$obj/$(echo "$f" | tr / _).o"
    [ "$o" -nt "$NAM/$f" ] && continue
    "$cxx" "$@" "${INC[@]}" "${DEFS[@]}" -c "$NAM/$f" -o "$o" &
    pids+=($!)
    if [ ${#pids[@]} -ge "$JOBS" ]; then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
  done
  for f in "${pids[@]}"; do wait "$f"; done
  "$cxx" "$@" "${INC[@]}" "${DEFS[@]}" -c "$HERE/src/nam_mpc.cpp" -o "$obj/nam_mpc.o"
}
LINK=(-shared -Wl,--version-script="$HERE/src/exports.map" -Wl,--exclude-libs,ALL -Wl,--no-undefined -Wl,-z,defs)

if [ "$MODE" = host ]; then
  FLAGS=(-std=c++20 -O1 -g -fPIC -fvisibility=hidden -fsanitize=address,undefined -fno-omit-frame-pointer)
  compile g++ "$B/host-obj" "${FLAGS[@]}"
  g++ "${FLAGS[@]}" "$B"/host-obj/*.o -shared -Wl,--version-script="$HERE/src/exports.map" -o "$B/NAM-MPC-host.so"
  g++ -std=c++20 -O1 -g -fsanitize=address,undefined "$HERE/test/nam_test.cpp" -ldl -o "$B/nam-test-host"
  "$HERE/test/make_fixture.sh" "$B/fixture" "$NAM/example_models"
  NAM_MPC_DIR="$B/fixture" ASAN_OPTIONS=detect_leaks=1 "$B/nam-test-host" "$B/NAM-MPC-host.so"
  exit
fi

# ---- ARM build -------------------------------------------------------------------------------------------------
# CXX: arm-linux-gnueabihf-g++ (cross), or plain g++ inside the arm32v7/gcc:12 container (CI)
arm() {
  local cxx="${CXX:-arm-linux-gnueabihf-g++}" strip="${STRIP:-arm-linux-gnueabihf-strip}" readelf="${READELF:-arm-linux-gnueabihf-readelf}"
  local flags=(-std=c++20 -O3 -ffast-math -fPIC -fvisibility=hidden -fvisibility-inlines-hidden
               -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17 -Wall -Wno-unused-parameter)
  compile "$cxx" "$B/arm-obj" "${flags[@]}"
  "$cxx" "${flags[@]}" "$B"/arm-obj/*.o "${LINK[@]}" -static-libstdc++ -static-libgcc -o "$B/NAM-MPC.so"
  "$cxx" -std=c++20 -O2 -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard "$HERE/test/nam_test.cpp" -ldl \
    -static-libstdc++ -static-libgcc -o "$B/nam-test"
  "$strip" "$B/NAM-MPC.so" "$B/nam-test"
  echo "exported: $("$readelf" --dyn-syms -W "$B/NAM-MPC.so" | awk '$5=="GLOBAL" && $7!="UND" {print $8}' | tr '\n' ' ')"
  echo "needs: $("$readelf" -d "$B/NAM-MPC.so" | grep NEEDED | grep -o '\[.*\]' | tr '\n' ' ')"
  echo "highest glibc: $("$readelf" -V "$B/NAM-MPC.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
}

# ---- skin: the MPC touchscreen page, drawn by mpc-vst-plugins' tools from params.json + layout.conf ---------------
skin() {
  local mv="$DEPS/mpc-vst-plugins-main" tools="$B/skin-tools"
  [ -x "$B/shadow_art" ] || gcc -O2 -I"$mv/tools/vendor/force-shadow/tools" -o "$B/shadow_art" "$mv/tools/shadow_art.c" -lm
  # a copy of the generator with every text size scaled up (skin/text_scale.py); frame titles in Titillium Bold
  rm -rf "$tools" && cp -a "$mv/tools" "$tools"
  python3 "$HERE/skin/text_scale.py" "$tools/shadow_skin.py" "${TEXT_SCALE:-1.35}"
  mkdir -p "$B/port/build"
  cp "$HERE/vst.json" "$HERE/params.json" "$HERE/layout.conf" "$B/port/"
  cp "$B/shadow_art" "$B/port/build/"
  (cd "$B/port" && SHADOW_TITLE_FONT="$mv/tools/html_art/fonts/TitilliumWeb-Bold.ttf" python3 "$tools/gen_vst.py" vst.json)
  rm -rf "$B/skin" && cp -a "$B/port/build/skin" "$B/skin"
  rm -f "$B"/preview_*.png
  python3 "$tools/studio.py" preview "$(ls -d "$B"/skin/*/)Plugin Skins" -o "$B/preview_%d.png" >/dev/null
  echo "skin: $(ls "$B/skin")"
}

# ---- release zip -----------------------------------------------------------------------------------------------
package() {
  local name="NAM-MPC-$VERSION_STR" d glibc
  d="$B/$name"
  glibc="$(readelf -V "$B/NAM-MPC.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/GLIBC_//')"
  rm -rf "$d" && mkdir -p "$d/payload/vst/NAM/Models" "$d/payload/vst/NAM/IRs" "$d/payload/Synths" "$d/licenses"
  cp "$B/NAM-MPC.so" "$d/payload/vst/"
  cp "$B/nam-test" "$d/"
  cp -a "$B"/skin/* "$d/payload/Synths/"
  cp "$HERE/release/uninstall.sh" "$HERE/release/plugin_list.awk" "$HERE/release/plugin.xml" "$d/"
  sed "s/@GLIBC@/$glibc/g; s/@VERSION@/$VERSION_STR/g" "$HERE/release/install.sh" > "$d/install.sh"
  sed "s/@GLIBC@/$glibc/g; s/@VERSION@/$VERSION_STR/g" "$HERE/release/INSTALL.md" > "$d/INSTALL.md"
  chmod +x "$d/install.sh" "$d/uninstall.sh" "$d/nam-test"
  printf 'Put .nam amp models here (sub-folders are fine).\n' > "$d/payload/vst/NAM/Models/PUT-MODELS-HERE.txt"
  printf 'Put .wav cab impulse responses here (sub-folders are fine).\n' > "$d/payload/vst/NAM/IRs/PUT-IRS-HERE.txt"
  cp "$NAM/LICENSE" "$d/licenses/NeuralAmpModelerCore-MIT.txt"
  cp "$DEPS/AudioDSPTools-main/LICENSE" "$d/licenses/AudioDSPTools-MIT.txt"
  cp "$DEPS/eigen-master/COPYING.MPL2" "$d/licenses/Eigen-MPL2.txt"
  (cd "$d" && find . -type f ! -name SHA256SUMS | sort | xargs -d '\n' sha256sum > SHA256SUMS)
  rm -f "$B/$name-mpc-armv7.zip"
  (cd "$B" && zip -qr "$name-mpc-armv7.zip" "$name")
  echo "release: $B/$name-mpc-armv7.zip (needs glibc $glibc)"
}

case "$MODE" in
  arm) arm ;;
  skin) skin ;;
  package) package ;;
  all) arm; skin; package ;;
  *) echo "usage: $0 [all|arm|skin|package|host|deps]" >&2; exit 2 ;;
esac
