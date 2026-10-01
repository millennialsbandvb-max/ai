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
# safety net around every audio block (applied once, also to an already unpacked tree): denormals flushed while the
# reverb runs; non-numbers in the input replaced by silence, input held under +12 dB; if the reverb ever puts out a
# non-number or blows up past +36 dB it is reset
# (deactivate + activate clears its tail) instead of passing it on, since one would silence MPC's whole mix; and
# outputs held under +12 dB
python3 - "$DEPS/DPF-main/distrho/src/DistrhoPluginVST2.cpp" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "MPC safety net v2" not in s:
    old = "        fPlugin.run(inputs, outputs, sampleFrames);\n      #endif"
    assert s.count(old) == 1, "DPF safety-net patch no longer applies"
    new = """        {   // MPC safety net v2
           #if defined(__arm__)
            uint32_t fpscr_saved_, fpscr_;
            __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr_saved_));
            fpscr_ = fpscr_saved_ | (1u << 24);
            __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr_));
           #endif
            const float* safeIn_[DISTRHO_PLUGIN_NUM_INPUTS > 0 ? DISTRHO_PLUGIN_NUM_INPUTS : 1];
            for (uint32_t c_ = 0; c_ < DISTRHO_PLUGIN_NUM_INPUTS; ++c_) {
                std::vector<float>& b_ = fSafeIn_[c_];
                if (b_.size() < (size_t)sampleFrames) b_.resize((size_t)sampleFrames);
                for (int32_t i_ = 0; i_ < sampleFrames; ++i_) {
                    const float v_ = inputs[c_][i_];
                    uint32_t u_; std::memcpy(&u_, &v_, 4);
                    b_[(size_t)i_] = ((u_ & 0x7f800000u) == 0x7f800000u) ? 0.0f : v_ > 4.0f ? 4.0f : v_ < -4.0f ? -4.0f : v_;
                }
                safeIn_[c_] = b_.data();
            }
            fPlugin.run(safeIn_, outputs, sampleFrames);
            bool bad_ = false;
            for (uint32_t c_ = 0; c_ < DISTRHO_PLUGIN_NUM_OUTPUTS; ++c_)
                for (int32_t i_ = 0; i_ < sampleFrames; ++i_) {
                    float v_ = outputs[c_][i_];
                    uint32_t u_; std::memcpy(&u_, &v_, 4);
                    if ((u_ & 0x7f800000u) == 0x7f800000u || v_ > 64.0f || v_ < -64.0f) { bad_ = true; v_ = 0.0f; }   // non-number, or blown up (+36 dB)
                    outputs[c_][i_] = v_ > 4.0f ? 4.0f : v_ < -4.0f ? -4.0f : v_;
                }
            if (bad_) {
                for (uint32_t c_ = 0; c_ < DISTRHO_PLUGIN_NUM_OUTPUTS; ++c_)
                    std::memset(outputs[c_], 0, sizeof(float) * (size_t)sampleFrames);
                fPlugin.deactivate();
                fPlugin.activate();
            }
           #if defined(__arm__)
            __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr_saved_));
           #endif
        }
      #endif"""
    s = s.replace(old, new)
    anchor = "private:\n    // Plugin\n    PluginExporter fPlugin;"
    assert s.count(anchor) == 1, "DPF member anchor no longer applies"
    s = s.replace(anchor, anchor + "\n    std::vector<float> fSafeIn_[DISTRHO_PLUGIN_NUM_INPUTS > 0 ? DISTRHO_PLUGIN_NUM_INPUTS : 1];   // MPC safety net")
    if "#include <vector>" not in s:
        s = s.replace("#include ", "#include <vector>\n#include <cstring>\n#include ", 1)
    open(p, "w").write(s)
    print("DPF: safety net added")
PY

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
