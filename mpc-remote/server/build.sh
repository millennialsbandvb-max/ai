#!/usr/bin/env bash
# Build mpc-remote: build/bundle/{bin,services} for patch_image.sh's EXTRA folder (ARM), and build/mpc-remote-test,
# a PC build that shows a BMP instead of the real screen and prints touches (MPC_REMOTE_FAKE_BMP=<file>).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HERE/build"
mkdir -p "$B"
python3 - "$HERE/index.html" "$B/index_html.h" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
with open(sys.argv[2], "w") as f:
    f.write("/* generated from index.html by build.sh */\nstatic const char INDEX_HTML[] = {\n")
    for i in range(0, len(data), 24):
        f.write("  " + ",".join(str(b) for b in data[i:i + 24]) + ",\n")
    f.write("  0};\n")
PY
gcc -O2 -Wall -Wextra -DMPC_REMOTE_FAKE -I"$B" -o "$B/mpc-remote-test" "$HERE/mpc-remote.c"
rm -rf "$B/bundle" && mkdir -p "$B/bundle/bin" "$B/bundle/services"
arm-linux-gnueabihf-gcc -O2 -Wall -Wextra -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17 \
  -I"$B" -o "$B/bundle/bin/mpc-remote" "$HERE/mpc-remote.c"
arm-linux-gnueabihf-strip "$B/bundle/bin/mpc-remote"
cp "$HERE/mpc-remote.service" "$HERE/mpc-cleanup.service" "$B/bundle/services/"
cp "$HERE/mpc-cleanup.sh" "$B/bundle/bin/"
echo "built $B/bundle"
