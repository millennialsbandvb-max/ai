#!/usr/bin/env bash
# Build MPC Browser (the page the MPC browser shows on, on the MPC's own screen).
#   ./build.sh [arm|skin|bundle]
#     arm: the plugin for the MPC, skin: its page (+ the marker area), bundle: build/bundle for patch_image.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
SYSROOT="${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}"
PAGE="MPC-Browser - VST - MPC Browser"
mkdir -p "$B"
case "$MODE" in
arm)
  arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -fPIC -shared \
    -fvisibility=hidden -I"$REPO/nam-mpc/src" "$HERE/src/browser_mpc.c" -Wl,--no-undefined \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/MPC-Browser.so"
  arm-linux-gnueabihf-strip "$B/MPC-Browser.so"
  echo "built $B/MPC-Browser.so" ;;
skin)
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/"
  # the browser's area: everything below the buttons (layout y 160..704 = background y 74..618: it starts at 86)
  python3 - "$B/skin/$PAGE/Plugin Skins/sh_bg_0.png" "$B"/preview_page_0.png <<'PY'
import sys
from PIL import Image, ImageDraw
for f in sys.argv[1:]:
    im = Image.open(f).convert("RGB")
    top = 160 - 86 if im.size[1] < 700 else 160      # the preview is drawn in layout coordinates
    ImageDraw.Draw(im).rectangle([20, top, 1259, top + 544 - 1], fill=(0x12, 0x34, 0x56))
    im.save(f)
PY
  ;;
bundle)
  "$0" arm && "$0" skin
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/MPC-Browser.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/MPC-Browser.so" /usr/lib/nam-mpc/plugins/MPC-Browser.so \
    > "$B/bundle/entries/MPC-Browser.xml"
  cat "$B/bundle/entries/MPC-Browser.xml"
  [ -d "$B/bundle/skins/$PAGE" ] || { echo "no page folder for MPC Browser" >&2; exit 1; } ;;
*) echo "usage: $0 [arm|skin|bundle]" >&2; exit 2 ;;
esac
