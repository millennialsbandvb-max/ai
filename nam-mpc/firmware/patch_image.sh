#!/usr/bin/env bash
# Make a USB-update firmware image that installs NAM MPC, from an MPC OS Gen1 update image (Akai's own, or a mod
# such as Hakai built on it) in the device-tree format, e.g. MPC-3.9.1-Gen1-update.img.
#   patch_image.sh <MPC update .img> <output .img>
#   patch_image.sh --rootfs <rootfs.ext4>          only add NAM MPC to an extracted root filesystem (for tests)
# Env: BUILD (nam-mpc/build: NAM-MPC.so and skin/ from build.sh).
# Nothing in the official image is changed except what NAM MPC adds:
#   /usr/lib/nam-mpc/{NAM-MPC.so, nam-mpc-boot.sh, plugin_list.awk, plugin.xml}
#   /usr/share/Akai/Content/Synths/NAM-MPC - VST - NAM MPC/   (its touchscreen page)
#   /etc/systemd/system/nam-mpc.service (+ its multi-user.target.wants link)
# Needs: python3, xz, e2fsprogs (debugfs, e2fsck), binutils (readelf, strings), and upx when MPC is UPX-packed.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$(dirname "$HERE")/build}"
SO="$BUILD/NAM-MPC.so"
SKIN_NAME="NAM-MPC - VST - NAM MPC"
SKIN="$BUILD/skin/$SKIN_NAME"
die() { echo "error: $*" >&2; exit 1; }
dbg() { debugfs -R "$1" "$ROOTFS" 2>/dev/null; }
exists() { grep -q '^Inode:' <<<"$(debugfs -R "stat \"$1\"" "$ROOTFS" 2>&1)"; }
# resolve <path>: follow symlinks (absolute or relative) inside the image; prints the real path
resolve() {
  local p="$1" i t
  for i in 1 2 3 4 5 6 7 8; do
    exists "$p" || return 1
    t="$(dbg "stat \"$p\"" | sed -n 's/^Fast link dest: "\(.*\)"$/\1/p')"
    [ -z "$t" ] && { echo "$p"; return 0; }
    case "$t" in /*) p="$t" ;; *) p="$(dirname "$p")/$t" ;; esac
  done
  return 1
}

check_rootfs() {
  echo "== checking the root filesystem"
  grep -q 'ext[234] filesystem' <<<"$(file -b "$ROOTFS")" || die "rootfs isn't ext2/3/4 ($(file -b "$ROOTFS"))"
  local tmp; tmp="$(mktemp -d)"
  exists /usr/bin/MPC || die "no /usr/bin/MPC: not an MPC OS image"
  dbg "dump /usr/bin/MPC $tmp/MPC" >/dev/null
  grep -q 'ARM$' <<<"$(readelf -h "$tmp/MPC")" || die "MPC isn't a 32-bit ARM program: this is not a Gen1 image (use the Gen1 update)"
  strings -n 4 "$tmp/MPC" > "$tmp/strings"
  if grep -q 'UPX!' "$tmp/strings"; then   # Hakai ships MPC UPX-packed: look inside a copy
    command -v upx >/dev/null || die "MPC is UPX-packed; install upx to check it"
    upx -d -q "$tmp/MPC" >/dev/null || die "couldn't unpack MPC to check it"
  fi
  strings -n 4 "$tmp/MPC" > "$tmp/strings"
  for s in KNOWNPLUGINS VSTPluginMain pluginList; do
    grep -qx "$s" "$tmp/strings" || grep -q "$s" "$tmp/strings" || die "MPC has no '$s': this firmware has no VST2 plugin host"
  done
  grep -q N4juce15VSTPluginFormatE "$tmp/strings" || die "MPC has no JUCE VST2 host"
  { exists /lib/systemd/system/acvs.service || exists /usr/lib/systemd/system/acvs.service || exists /etc/systemd/system/acvs.service; } ||
    die "no acvs.service (the unit that runs MPC)"
  exists /usr/share/Akai/Content/Synths || die "no /usr/share/Akai/Content/Synths"
  { exists /usr/bin/awk || exists /bin/awk; } || die "no awk on the device"
  local libc need have
  libc="$(resolve /lib/libc.so.6)" || libc="$(resolve /usr/lib/libc.so.6)" || die "no libc.so.6 in the firmware"
  dbg "dump \"$libc\" $tmp/libc" >/dev/null
  have="$(strings "$tmp/libc" | grep -oE 'GNU C Library.* version [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+$' | head -n 1)"
  need="$(readelf -V "$SO" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 | sed 's/GLIBC_//')"
  echo "glibc: firmware ${have:-unknown}, plugin needs $need"
  [ -n "$have" ] && [ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | head -n 1)" = "$need" ] ||
    die "the plugin needs glibc $need but this firmware has ${have:-unknown}"
  rm -rf "$tmp"
}

inject() {
  [ -f "$SO" ] || die "$SO missing: run build.sh first"
  [ -f "$SKIN/Plugin Skins/TUI.json" ] || die "$SKIN missing: run build.sh skin first"
  check_rootfs
  local free need
  free=$(dbg stats | awk -F: '/^Free blocks:/ {f=$2} /^Block size:/ {b=$2} END {print f*b}')
  need=$(( $(du -sb "$SO" | cut -f1) + $(du -sb "$SKIN" | cut -f1) + 1048576 ))
  echo "free space: $free bytes, NAM MPC needs about $need"
  [ "$free" -gt $((need * 2)) ] || die "not enough free space in the root filesystem"
  exists /usr/lib/nam-mpc && die "this image already has NAM MPC"

  echo "== adding NAM MPC"
  local cmds f rel d syn="/usr/share/Akai/Content/Synths/$SKIN_NAME"
  cmds="$(mktemp)"
  own() { echo "sif \"$1\" uid 0"; echo "sif \"$1\" gid 0"; echo "sif \"$1\" mode $2"; }
  mkd() { echo "mkdir \"$1\""; own "$1" 040755; }
  put() { echo "write \"$1\" \"$2\""; own "$2" "$3"; }   # <local file> <image path> <mode>
  {
    mkd /usr/lib/nam-mpc
    put "$SO" /usr/lib/nam-mpc/NAM-MPC.so 0100755
    put "$HERE/nam-mpc-boot.sh" /usr/lib/nam-mpc/nam-mpc-boot.sh 0100755
    put "$HERE/../release/plugin_list.awk" /usr/lib/nam-mpc/plugin_list.awk 0100644
    put "$HERE/plugin.xml" /usr/lib/nam-mpc/plugin.xml 0100644
    exists /etc/systemd/system/multi-user.target.wants || mkd /etc/systemd/system/multi-user.target.wants
    put "$HERE/nam-mpc.service" /etc/systemd/system/nam-mpc.service 0100644
    echo "symlink /etc/systemd/system/multi-user.target.wants/nam-mpc.service /etc/systemd/system/nam-mpc.service"
    mkd "$syn"
    (cd "$SKIN" && find . -mindepth 1 -type d | sort) | while read -r d; do mkd "$syn/${d#./}"; done
    (cd "$SKIN" && find . -type f | sort) | while read -r rel; do put "$SKIN/${rel#./}" "$syn/${rel#./}" 0100644; done
  } > "$cmds"
  debugfs -w -f "$cmds" "$ROOTFS" > "$cmds.log" 2>&1
  if grep -iE 'error|not found|could not|failed|exists' "$cmds.log" | grep -v '^debugfs'; then die "debugfs reported problems (above)"; fi
  rm -f "$cmds" "$cmds.log"

  echo "== verifying"
  e2fsck -fn "$ROOTFS" >/dev/null || die "filesystem check failed after the edit"
  local tmp; tmp="$(mktemp -d)"
  dbg "dump /usr/lib/nam-mpc/NAM-MPC.so $tmp/so" >/dev/null
  cmp -s "$tmp/so" "$SO" || die "plugin read back differs"
  dbg "dump \"/usr/share/Akai/Content/Synths/$SKIN_NAME/Plugin Skins/TUI.json\" $tmp/tui" >/dev/null
  cmp -s "$tmp/tui" "$SKIN/Plugin Skins/TUI.json" || die "skin read back differs"
  grep -q 'Fast link dest: "/etc/systemd/system/nam-mpc.service"' <<<"$(dbg "stat /etc/systemd/system/multi-user.target.wants/nam-mpc.service")" ||
    die "service link missing"
  grep -q 'Mode:  0755' <<<"$(dbg "stat /usr/lib/nam-mpc/nam-mpc-boot.sh")" || die "boot script isn't executable"
  rm -rf "$tmp"
  echo "root filesystem OK"
}

if [ "${1:-}" = --rootfs ]; then
  ROOTFS="$2"
  inject
  exit
fi

[ $# = 2 ] || die "usage: $0 <MPC update .img> <output .img>"
IN="$1"; OUT="$2"
FIT="python3 $HERE/mpcfit.py"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/nam-fw.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
echo "== input image"
$FIT info "$IN" | grep -v '^properties'
echo "== unpacking the root filesystem"
$FIT extract "$IN" "$WORK/rootfs.xz"
grep -q 'CRC64' <<<"$(xz -lvv "$WORK/rootfs.xz")" || die "unexpected xz check type"
xz -dc "$WORK/rootfs.xz" > "$WORK/rootfs.ext4"
rm "$WORK/rootfs.xz"
ROOTFS="$WORK/rootfs.ext4"
inject

echo "== building the new image (xz -6, one block, CRC64: the same settings as the original)"
xz -6 -T1 -C crc64 -c "$ROOTFS" > "$WORK/new.xz"
$FIT build "$IN" "$WORK/new.xz" "$OUT"
echo "== checking the new image"
$FIT info "$OUT" | grep -v '^properties' > "$WORK/out-info.txt"
diff <($FIT info "$IN" | grep -vE 'rootfs (data|sha1)') <($FIT info "$OUT" | grep -vE 'rootfs (data|sha1)') ||
  die "the new image's header differs from the input's beyond the rootfs"
$FIT extract "$OUT" "$WORK/check.xz"
xz -t "$WORK/check.xz" || die "the new rootfs doesn't decompress"
xz -dc "$WORK/check.xz" | cmp -s - "$ROOTFS" || die "the new image doesn't unpack to the patched filesystem"
echo "new image: $OUT"
sha256sum "$OUT"
