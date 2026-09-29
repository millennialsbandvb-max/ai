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

if [ "$MODE" = skin ]; then
  # the same page generator and larger text as NAM MPC (nam-mpc/build.sh skin), one page per plugin
  mv="$DEPS/mpc-vst-plugins-main" tools="$B/skin-tools"
  [ -d "$mv" ] || unzip -q "$REPO/mpc-vst-plugins-main.zip" -d "$DEPS"
  rm -rf "$tools" && cp -a "$mv/tools" "$tools"
  python3 "$REPO/nam-mpc/skin/text_scale.py" "$tools/shadow_skin.py" "${TEXT_SCALE:-1.35}"
  # choice buttons (Plate's algorithm, Early Reflections' program): taller, with a real font at a readable size
  patch() {   # patch <file> <old> <new>: exactly one match, or stop
    [ "$(grep -cF -- "$2" "$1")" = 1 ] || { echo "page patch no longer applies to $1: $2" >&2; exit 1; }
    python3 -c 'import sys; p, o, n = sys.argv[1:]; s = open(p).read(); open(p, "w").write(s.replace(o, n))' "$1" "$2" "$3"
  }
  patch "$tools/shadow_skin.py" 'sw, sh, gap = w.get("sw") or 117, 33, 2' 'sw, sh, gap = w.get("sw") or 117, 44, 2'
  patch "$tools/shadow_art.c" 'draw_text_c(x + w / 2, y + h / 2 - 6, a[7], 1.15f, HEX(a[6]));' \
        'label_text_c(x + w / 2, y + h / 2 - 15, a[7], 2.1f, HEX(a[6]));'   # label_text_c: the real font
  rm -f "$B/shadow_art"
  gcc -O2 -I"$mv/tools/vendor/force-shadow/tools" -o "$B/shadow_art" "$tools/shadow_art.c" -lm
  rm -rf "$B/skin" "$B"/preview_*.png && mkdir -p "$B/skin"
  for d in "$HERE"/pages/*/; do
    p=$(basename "$d"); w="$B/port-$p"
    rm -rf "$w" && mkdir -p "$w/build" && cp "$d"/* "$w/" && cp "$B/shadow_art" "$w/build/"
    { echo "font_label=$mv/tools/html_art/fonts/TitilliumWeb-SemiBold.ttf"; cat "$d/layout.conf"; } > "$w/layout.conf"
    (cd "$w" && SHADOW_TITLE_FONT="$mv/tools/html_art/fonts/TitilliumWeb-Bold.ttf" python3 "$tools/gen_vst.py" vst.json >/dev/null)
    cp -a "$w"/build/skin/* "$B/skin/"
    python3 "$tools/studio.py" preview "$(ls -d "$w"/build/skin/*/)Plugin Skins" -o "$B/preview_${p}_%d.png" >/dev/null
    echo "page: $(ls "$w/build/skin")"
  done
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
