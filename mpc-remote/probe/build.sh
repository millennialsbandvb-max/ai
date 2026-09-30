#!/usr/bin/env bash
# Build the MPC probe for the firmware: build/bundle/{bin,services}, to merge into patch_image.sh's EXTRA folder.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HERE/build/bundle"
rm -rf "$B" && mkdir -p "$B/bin" "$B/services"
arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -o "$B/bin/mpc-probe" "$HERE/mpc-probe.c"
arm-linux-gnueabihf-strip "$B/bin/mpc-probe"
cp "$HERE/mpc-probe.sh" "$B/bin/"
cp "$HERE/mpc-probe.service" "$B/services/"
echo "built $B"
