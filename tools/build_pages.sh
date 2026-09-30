#!/usr/bin/env bash
# Build MPC touchscreen pages for our plugins, in one shared style: 1.35x text (nam-mpc/skin/text_scale.py), frame
# titles in Titillium Bold, and choice buttons 44 px tall with Titillium SemiBold labels.
#   tools/build_pages.sh <work dir> <out skin dir> <page dir>...
# Each <page dir> holds a vst.json, params.json and layout.conf (mpc-vst-plugins' formats). The page folders land in
# <out skin dir>, and a preview of each page in <work dir>/preview_<page>_<n>.png.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
W="$1"; OUT="$2"; shift 2
DEPS="$W/deps"
mv="$DEPS/mpc-vst-plugins-main" tools="$W/skin-tools"
mkdir -p "$DEPS"
[ -d "$mv" ] || unzip -q "$REPO/mpc-vst-plugins-main.zip" -d "$DEPS"
rm -rf "$tools" && cp -a "$mv/tools" "$tools"
python3 "$REPO/nam-mpc/skin/text_scale.py" "$tools/shadow_skin.py" "${TEXT_SCALE:-1.35}" >/dev/null
patch() {   # patch <file> <old> <new>: exactly one match, or stop
  [ "$(grep -cF -- "$2" "$1")" = 1 ] || { echo "page patch no longer applies to $1: $2" >&2; exit 1; }
  python3 -c 'import sys; p, o, n = sys.argv[1:]; s = open(p).read(); open(p, "w").write(s.replace(o, n))' "$1" "$2" "$3"
}
patch "$tools/shadow_skin.py" 'sw, sh, gap = w.get("sw") or 117, 33, 2' 'sw, sh, gap = w.get("sw") or 117, 44, 2'
patch "$tools/shadow_art.c" 'draw_text_c(x + w / 2, y + h / 2 - 6, a[7], 1.15f, HEX(a[6]));' \
      'label_text_c(x + w / 2, y + h / 2 - 15, a[7], 2.1f, HEX(a[6]));'   # label_text_c: the real font
gcc -O2 -I"$mv/tools/vendor/force-shadow/tools" -o "$W/shadow_art" "$tools/shadow_art.c" -lm
rm -rf "$OUT" "$W"/preview_*.png && mkdir -p "$OUT"
for d in "$@"; do
  p=$(basename "$d"); w="$W/port-$p"
  rm -rf "$w" && mkdir -p "$w/build" && cp "$d"/* "$w/" && cp "$W/shadow_art" "$w/build/"
  { echo "font_label=$mv/tools/html_art/fonts/TitilliumWeb-SemiBold.ttf"; cat "$d/layout.conf"; } > "$w/layout.conf"
  (cd "$w" && SHADOW_TITLE_FONT="$mv/tools/html_art/fonts/TitilliumWeb-Bold.ttf" python3 "$tools/gen_vst.py" vst.json >/dev/null)
  cp -a "$w"/build/skin/* "$OUT/"
  python3 "$tools/studio.py" preview "$(ls -d "$w"/build/skin/*/)Plugin Skins" -o "$W/preview_${p}_%d.png" >/dev/null
  echo "page: $(ls "$w/build/skin")"
done
