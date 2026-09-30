#!/usr/bin/env bash
# Build SFZ MPC (sfizz, an SFZ sample player, as a VST2 instrument for MPC OS).
#   ./build.sh [host|arm|test|skin|bundle]
#     host: x86 build + test host, arm (default): 32-bit ARM build for the MPC (+ ARM test host for qemu),
#     test: run the offline test on both (SFZ_MPC_DIR=<folder with .sfz instruments>, ARM_SYSROOT=<MPC rootfs dump>),
#     skin: the MPC touchscreen page (build/skin/, preview build/preview_page_0.png),
#     bundle: build/bundle/{plugins,skins,entries} for nam-mpc/firmware/patch_image.sh (EXTRA=...)
# Source: sfizz-1.2.3.tar.gz at the repo root (BSD-2-Clause), built as static libraries with CMake.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
MODE="${1:-arm}"
B="$HERE/build"
S="$B/deps/sfizz-1.2.3"
SYSROOT="${ARM_SYSROOT:-/usr/arm-linux-gnueabihf}"
ARMF=(-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a17)

mkdir -p "$B/deps"
[ -d "$S" ] || tar -xzf "$REPO/sfizz-1.2.3.tar.gz" -C "$B/deps"

sfizz_libs() {   # <build dir> [cmake args...]: configure + build sfizz's static libraries once
  local d="$1"; shift
  [ -f "$d/library/lib/libsfizz.a" ] && return 0
  cmake -S "$S" -B "$d" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSFIZZ_JACK=OFF -DSFIZZ_RENDER=OFF -DSFIZZ_SHARED=OFF \
    -DSFIZZ_GIT_SUBMODULE_CHECK=OFF -DENABLE_LTO=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" > "$d.cfg.log"
  cmake --build "$d" > "$d.build.log"
}

case "$MODE" in
host)
  sfizz_libs "$B/lib-host"
  g++ -std=c++17 -O2 -g -fPIC -shared -fvisibility=hidden -I"$HERE/src" -I"$REPO/nam-mpc/src" -I"$S/src" \
    "$HERE/src/sfizz_mpc.cpp" -Wl,--start-group "$B"/lib-host/library/lib/*.a -Wl,--end-group -lpthread -ldl \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/SFZ-MPC-host.so"
  g++ -std=c++17 -O2 -I"$REPO/nam-mpc/src" "$HERE/test/sfz_test.cpp" -ldl -lpthread -o "$B/sfz-test-host"
  echo "built $B/SFZ-MPC-host.so" ;;
arm)
  sfizz_libs "$B/lib-arm" -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/armhf-toolchain.cmake"
  arm-linux-gnueabihf-g++ -std=c++17 -O2 "${ARMF[@]}" -fPIC -shared -fvisibility=hidden -I"$HERE/src" \
    -I"$REPO/nam-mpc/src" -I"$S/src" "$HERE/src/sfizz_mpc.cpp" \
    -Wl,--start-group "$B"/lib-arm/library/lib/*.a -Wl,--end-group -lpthread -ldl \
    -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,--no-undefined \
    -Wl,--version-script="$REPO/nam-mpc/src/exports.map" -o "$B/SFZ-MPC.so"
  arm-linux-gnueabihf-strip "$B/SFZ-MPC.so"
  arm-linux-gnueabihf-g++ -std=c++17 -O2 "${ARMF[@]}" -I"$REPO/nam-mpc/src" "$HERE/test/sfz_test.cpp" \
    -static-libstdc++ -static-libgcc -ldl -lpthread -o "$B/sfz-test-arm"
  echo "built $B/SFZ-MPC.so" ;;
test)
  : "${SFZ_MPC_DIR:?set SFZ_MPC_DIR to a folder with .sfz instruments}"
  "$B/sfz-test-host" "$B/SFZ-MPC-host.so"
  qemu-arm -L "$SYSROOT" "$B/sfz-test-arm" "$B/SFZ-MPC.so" ;;
skin)
  "$REPO/tools/build_pages.sh" "$B" "$B/skin" "$HERE/page/" ;;
bundle)
  [ -f "$B/SFZ-MPC.so" ] && [ -d "$B/skin" ] || { echo "run ./build.sh arm and ./build.sh skin first" >&2; exit 1; }
  arm-linux-gnueabihf-gcc -O2 -Wall -o "$B/vst-probe-arm" "$REPO/nam-mpc/firmware/vst-probe.c" -ldl
  rm -rf "$B/bundle" && mkdir -p "$B/bundle/plugins" "$B/bundle/skins" "$B/bundle/entries"
  cp "$B/SFZ-MPC.so" "$B/bundle/plugins/"
  cp -a "$B"/skin/* "$B/bundle/skins/"
  qemu-arm -L "$SYSROOT" "$B/vst-probe-arm" "$B/bundle/plugins/SFZ-MPC.so" /usr/lib/nam-mpc/plugins/SFZ-MPC.so \
    > "$B/bundle/entries/SFZ-MPC.xml"
  cat "$B/bundle/entries/SFZ-MPC.xml"
  [ -d "$B/bundle/skins/SFZ-MPC - VST - SFZ MPC" ] || { echo "no page folder for SFZ MPC" >&2; exit 1; } ;;
*) echo "usage: $0 [host|arm|test|skin|bundle]" >&2; exit 2 ;;
esac
