#!/usr/bin/env bash
# Build the complete MPC Live II firmware from the user's Hakai image, in one go:
#   tools/build_firmware.sh <Hakai MPC-<version>-Gen1-update.img> <output dir>
# The result is named after the image's MPC OS version: MPC-<version>-Gen1-update.img (+ .part1 to .part7).
# Builds every plugin for ARM (NAM, Dragonfly, SFZ, Airwindows, Duo FX, Tri Chorus, MPC Browser) with their touchscreen pages, the browser remote,
# puts them into the image with nam-mpc/firmware/patch_image.sh, and splits the result into 7 pieces under 30 MiB
# (the most a single file can be when sent to the user in chat). Prints each piece's name and the image's SHA-256.
# Needs: arm-linux-gnueabihf-gcc/g++, cmake, ninja, qemu-arm, python3 (+PIL), e2fsprogs, xz, upx, unzip.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
IN="$(realpath "$1")"
OUT="$(mkdir -p "$2" && realpath "$2")"
W="$OUT/work"
mkdir -p "$W"
VER="$(python3 "$REPO/nam-mpc/firmware/mpcfit.py" info "$IN" | sed -n 's/^\/:inmusic,version *//p')"
[ -n "$VER" ] || { echo "can't read the image's MPC OS version" >&2; exit 1; }
IMG="MPC-$VER-Gen1-update.img"
# a sysroot unpacked from another image would test the plugins against the wrong libraries
[ "$(cat "$W/sysroot.from" 2>/dev/null)" = "$(sha256sum < "$IN")" ] || rm -rf "$W/sysroot"

# The image's own libraries, so the ARM plugin probes (which write each plugin's MPC.settings entry) run under
# qemu exactly as on the MPC.
if [ ! -f "$W/sysroot/usr/lib/ld-linux-armhf.so.3" ]; then
  echo "== unpacking the image's libraries"
  python3 "$REPO/nam-mpc/firmware/mpcfit.py" extract "$IN" "$W/rootfs.xz"
  xz -dc "$W/rootfs.xz" > "$W/rootfs.ext4" && rm "$W/rootfs.xz"
  rm -rf "$W/sysroot" && mkdir -p "$W/sysroot"
  debugfs -R "rdump /usr $W/sysroot" "$W/rootfs.ext4" >/dev/null 2>&1
  ln -sfn usr/lib "$W/sysroot/lib"
  rm "$W/rootfs.ext4"
  sha256sum < "$IN" > "$W/sysroot.from"
fi
export ARM_SYSROOT="$W/sysroot"

echo "== NAM"
(cd "$REPO/nam-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null)
echo "== Dragonfly"
(cd "$REPO/dragonfly-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null 2>&1 && ./build.sh bundle >/dev/null)
echo "== SFZ (sax player)"
(cd "$REPO/sfizz-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null 2>&1 && ./build.sh bundle >/dev/null)
echo "== Airwindows"
(cd "$REPO/airwindows-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null 2>&1 && ./build.sh bundle >/dev/null)
echo "== Duo FX (H90-style dual effect)"
(cd "$REPO/duofx-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null 2>&1 && ./build.sh bundle >/dev/null)
echo "== Tri Chorus"
(cd "$REPO/trichorus-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null 2>&1 && ./build.sh bundle >/dev/null)
echo "== vocal plugins (VoxLive 3X, HedRush Harmony, Vocal Rack)"
(cd "$REPO/vocal-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null 2>&1 && ./build.sh bundle >/dev/null)
echo "== Ambient Lab"
(cd "$REPO/ambient-mpc" && ./build.sh arm >/dev/null && ./build.sh skin >/dev/null 2>&1 && ./build.sh bundle >/dev/null)
echo "== MPC Browser (the browser's page on the MPC's screen)"
(cd "$REPO/browser-mpc" && ./build.sh bundle >/dev/null 2>&1)
echo "== browser remote"
(cd "$REPO/mpc-remote/server" && ./build.sh >/dev/null)

echo "== one bundle of everything"
rm -rf "$W/extra" && mkdir -p "$W/extra"
for b in dragonfly-mpc/build/bundle sfizz-mpc/build/bundle airwindows-mpc/build/bundle duofx-mpc/build/bundle \
         trichorus-mpc/build/bundle vocal-mpc/build/bundle ambient-mpc/build/bundle \
         browser-mpc/build/bundle \
         mpc-remote/server/build/bundle; do
  cp -a "$REPO/$b/." "$W/extra/"
done
echo "plugins: $(ls "$W/extra/plugins" | tr '\n' ' ')"

echo "== the image"
rm -f "$OUT/$IMG"
EXTRA="$W/extra" "$REPO/nam-mpc/firmware/patch_image.sh" "$IN" "$OUT/$IMG" | grep -E 'root filesystem OK|wrote'

echo "== 7 pieces"
rm -f "$OUT"/MPC-*-Gen1-update.img.part*
(cd "$OUT" && split -n 7 -d -a 1 "$IMG" piece. &&
  for i in 0 1 2 3 4 5 6; do mv "piece.$i" "$IMG.part$((i + 1))"; done)
ls -l "$OUT/$IMG".part*
echo "size $(stat -c %s "$OUT/$IMG") bytes, SHA-256 $(sha256sum "$OUT/$IMG" | cut -d' ' -f1)"
