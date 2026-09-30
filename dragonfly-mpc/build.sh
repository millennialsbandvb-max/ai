#!/usr/bin/env bash
# Build the Dragonfly Reverb plugins (Hall, Room, Plate, Early Reflections) as VST2 effects for MPC OS.
#   ./build.sh [host|arm|skin|bundle]
#     host: x86 builds for testing, arm (default): 32-bit ARM builds for the MPC,
#     skin: the four MPC touchscreen pages (build/skin/, previews build/preview_*.png),
#     bundle: build/bundle/{plugins,skins,entries} for nam-mpc/firmware/patch_image.sh (EXTRA=...): the ARM plugins,
#             their pages, and their MPC.settings entries, written by running vst-probe on each ARM plugin (qemu-arm)
# Sources: dragonfly-reverb-master.zip and DPF-main.zip at the repo root. Only each plugin's audio code is built
# (DPF drops the desktop editor without its graphics library); the MPC draws its own page instead.
# Each plugin's source list is read from its own Makefile (FILES_COMMON + FILES_DSP), as Dragonfly builds it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
DEPS="$B/deps"
D="$DEPS/dragonfly-reverb-master"
PLUGINS="dragonfly-hall-reverb dragonfly-room-reverb dragonfly-plate-reverb dragonfly-early-reflections"

mkdir -p "$DEPS"
if [ ! -d "$D" ]; then
  unzip -q "$REPO/dragonfly-reverb-master.zip" -d "$DEPS"
  unzip -q "$REPO/DPF-main.zip" -d "$DEPS"
  rmdir "$D/dpf" 2>/dev/null || true
  ln -sfn ../DPF-main "$D/dpf"
  # value text for the MPC's knob labels: DPF prints every value with %f ("80.000000"); use 0-2 decimals instead
  v2="$DEPS/DPF-main/distrho/src/DistrhoPluginVST2.cpp"
  [ "$(grep -c 'snprintf_f32((char\*)ptr, value, 24);' "$v2")" = 1 ] || { echo "DPF value-text patch no longer applies" >&2; exit 1; }
  sed -i 's|snprintf_f32((char\*)ptr, value, 24);|{ const float a_ = std::fabs(value); std::snprintf((char*)ptr, 24, a_ >= 100.f ? "%.0f" : a_ >= 10.f ? "%.1f" : "%.2f", value); }|' "$v2"
fi

# sources <plugin dir>: the .c/.cpp files in FILES_COMMON and FILES_DSP (not FILES_UI), relative to that dir
sources() {
  awk '
    /^FILES_(COMMON|DSP)[ \t]*[+:]?=/ { on = 1 }
    /^FILES_UI/ { on = 0 }
    on { for (i = 1; i <= NF; i++) if ($i ~ /\.(c|cpp)$/) print $i }
    on && !/\\[ \t]*$/ && !/^FILES_/ { on = 0 }
  ' "$1/Makefile" | sort -u
}

if [ "$MODE" = skin ]; then   # the shared page style (tools/build_pages.sh), one page per plugin
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE"/pages/*/
  exit 0
fi

if [ "$MODE" = bundle ]; then
  [ -d "$B/skin" ] && ls "$B"/arm/*.so >/dev/null || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B"/arm/*.so "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  for so in "$B"/bundle/plugins/*.so; do
    n=$(basename "$so" .so)
    qemu-arm -L "${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}" "$B/vst-probe-arm" "$so" "/usr/lib/nam-mpc/plugins/$n.so" \
      > "$B/bundle/entries/$n.xml"
    skin=$(python3 -c 'import sys, xml.etree.ElementTree as E; a = E.parse(sys.argv[1]).getroot().attrib
print("%s - VST - %s" % (a["manufacturer"], a["name"]))' "$B/bundle/entries/$n.xml")
    [ -d "$B/bundle/skins/$skin" ] || { echo "$n: no page folder named \"$skin\"" >&2; exit 1; }
    echo "$n: entry + page \"$skin\""
  done
  exit 0
fi

if [ "$MODE" = host ]; then
  CXX=g++; CC=gcc; OUT="$B/host"
  FLAGS=(-O2 -g -fPIC)
  LINKX=()
else
  CXX="${CXX:-arm-linux-gnueabihf-g++}"; CC="${CC:-arm-linux-gnueabihf-gcc}"; OUT="$B/arm"
  FLAGS=(-O3 -ffast-math -fPIC -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)
  LINKX=(-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL)
fi
mkdir -p "$OUT"
for p in $PLUGINS; do
  dir="$D/plugins/$p"
  name=$(sed -n 's/^NAME[ \t]*=[ \t]*//p' "$dir/Makefile")
  obj="$OUT/obj-$p"; mkdir -p "$obj"
  objs=()
  for f in $(sources "$dir") ../../dpf/distrho/DistrhoPluginMain.cpp; do
    src="$dir/$f"; o="$obj/$(echo "$f" | tr /. __).o"
    case "$f" in
      *.c) "$CC" "${FLAGS[@]}" -fvisibility=hidden -c "$src" -o "$o" ;;
      *) "$CXX" -std=gnu++14 "${FLAGS[@]}" -fvisibility=hidden -fvisibility-inlines-hidden -Wno-deprecated \
           -DDISTRHO_PLUGIN_TARGET_VST2 -DLIBFV3_FLOAT -I"$dir" -I"$D/common" -I"$D/dpf/distrho" -I"$D/dpf/distrho/src" \
           -c "$src" -o "$o" ;;
    esac &
    objs+=("$o")
  done
  wait
  "$CXX" -shared "${FLAGS[@]}" "${objs[@]}" "${LINKX[@]}" -Wl,--no-undefined -lm -o "$OUT/$name.so"
  echo "built $OUT/$name.so"
done
