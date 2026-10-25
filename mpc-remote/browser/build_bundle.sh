#!/usr/bin/env bash
# Build MPC-Browser.tar.gz: Chromium (Debian 12's build for 32-bit ARM) with the libraries the MPC doesn't have, a
# font and a start script. The user puts it on a card; the remote page installs it into /data/mpc-browser (cards
# are mounted noexec, so it can't run from there) and starts it.
#   build_bundle.sh <out dir>
# Downloads the packages in packages.txt from deb.debian.org and checks each against its SHA-256 there.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(mkdir -p "$1" && cd "$1" && pwd)"
W="$OUT/work"
mkdir -p "$W/debs"
rm -rf "$W/root" "$W/bundle" && mkdir -p "$W/root" "$W/bundle"

echo "== packages"
grep -v '^#' "$HERE/packages.txt" | while read -r f sum; do
  d="$W/debs/$(basename "$f")"
  base=https://deb.debian.org/debian
  case "$f" in pool/updates/*) base=https://deb.debian.org/debian-security ;; esac
  [ -f "$d" ] && [ "$(sha256sum < "$d" | cut -d' ' -f1)" = "$sum" ] || curl -sSfL -m 600 -o "$d" "$base/$f"
  [ "$(sha256sum < "$d" | cut -d' ' -f1)" = "$sum" ] || { echo "checksum mismatch: $f" >&2; exit 1; }
  dpkg-deb -x "$d" "$W/root"
done

echo "== layout"
B="$W/bundle"
mkdir -p "$B/lib" "$B/share" "$B/home"
cp -a "$W/root/usr/lib/chromium" "$B/chromium"
# not needed headless without a GPU, or in English
rm -f "$B"/chromium/libVk* "$B"/chromium/*.TOC "$B/chromium/vk_swiftshader_icd.json"
find "$B/chromium/locales" -type f ! -name 'en-US*' -delete
for d in "$W/root/usr/lib/arm-linux-gnueabihf" "$W/root/lib/arm-linux-gnueabihf"; do
  [ -d "$d" ] && cp -a "$d"/. "$B/lib/"
done
rm -rf "$B/lib/gdk-pixbuf-2.0" "$B/lib/gio" "$B/lib/glib-2.0"
cp -a "$W/root/usr/share/fonts" "$B/share/fonts"
cp "$HERE/run.sh" "$B/run.sh"
cp "$HERE/asoundrc" "$B/home/.asoundrc"
chmod 755 "$B/run.sh"

echo "== MPC-Browser.tar.gz"
(cd "$B" && tar -czf "$OUT/MPC-Browser.tar.gz" --owner=0 --group=0 .)
du -sh "$B" | sed 's/^/unpacked: /'
ls -l "$OUT/MPC-Browser.tar.gz"
sha256sum "$OUT/MPC-Browser.tar.gz"
