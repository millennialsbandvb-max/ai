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
python3 "$REPO/tools/page_lists.py" "$tools/shadow_skin.py"   # steppers with a list open it, not the knob menu
python3 "$REPO/nam-mpc/skin/text_scale.py" "$tools/shadow_skin.py" "${TEXT_SCALE:-1.35}" >/dev/null
patch() {   # patch <file> <old> <new>: exactly one match, or stop
  [ "$(grep -cF -- "$2" "$1")" = 1 ] || { echo "page patch no longer applies to $1: $2" >&2; exit 1; }
  python3 -c 'import sys; p, o, n = sys.argv[1:]; s = open(p).read(); open(p, "w").write(s.replace(o, n))' "$1" "$2" "$3"
}
patch "$tools/shadow_skin.py" 'sw, sh, gap = w.get("sw") or 117, 33, 2' 'sw, sh, gap = w.get("sw") or 117, 44, 2'
patch "$tools/shadow_art.c" 'draw_text_c(x + w / 2, y + h / 2 - 6, a[7], 1.15f, HEX(a[6]));' \
      'label_text_c(x + w / 2, y + h / 2 - 15, a[7], 2.1f, HEX(a[6]));'   # label_text_c: the real font
gcc -O2 -I"$mv/tools/vendor/force-shadow/tools" -o "$W/shadow_art" "$tools/shadow_art.c" -lm
# the pedal look (tools/pedal_look.*): pages drawn by the kit's browser renderer when Playwright is there
# (PEDAL_LOOK=0: the plain renderer)
art_env=()
if [ "${PEDAL_LOOK:-1}" = 1 ] && "${PEDAL_PY:-/usr/bin/python3}" -c "import playwright, PIL" 2>/dev/null; then
  python3 "$REPO/tools/pedal_look.py" "$tools/html_art.py"
  printf '#!/bin/sh\nexec %s %s "$@"\n' "${PEDAL_PY:-/usr/bin/python3}" "$tools/html_art.py" > "$W/html_art_run"
  chmod +x "$W/html_art_run"
  art_env=(SHADOW_ART="$W/html_art_run" PW_CHROMIUM="${PW_CHROMIUM:-$( [ -e /opt/pw-browsers/chromium ] && echo /opt/pw-browsers/chromium)}")
  echo "pages: pedal look (browser renderer)"
fi
rm -rf "$OUT" "$W"/preview_*.png && mkdir -p "$OUT"
for d in "$@"; do
  p=$(basename "$d"); w="$W/port-$p"
  rm -rf "$w" && mkdir -p "$w/build" && cp "$d"/* "$w/" && cp "$W/shadow_art" "$w/build/"
  { echo "font_label=$mv/tools/html_art/fonts/TitilliumWeb-SemiBold.ttf"; [ ${#art_env[@]} = 0 ] || echo "art_css=$REPO/tools/pedal_look.css"
    cat "$d/layout.conf"; } > "$w/layout.conf"
  (cd "$w" && env "${art_env[@]}" SHADOW_TITLE_FONT="$mv/tools/html_art/fonts/TitilliumWeb-Bold.ttf" python3 "$tools/gen_vst.py" vst.json >/dev/null)
  cp -a "$w"/build/skin/* "$OUT/"
  python3 "$tools/studio.py" preview "$(ls -d "$w"/build/skin/*/)Plugin Skins" -o "$W/preview_${p}_%d.png" >/dev/null
  echo "page: $(ls "$w/build/skin")"
done
