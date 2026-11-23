#!/bin/sh
# Our MPC OS 3.10.1 update image: Akai's official image with one addition, a boot service that starts HAKAI's full
# mode and our add-on from the drive (mpc-remote/runtime/boot.sh -> OtherThings/start.sh), so the MPC boots
# straight into HAKAI with no question. Everything else (HAKAI, OtherThings, VST2 Plugins, NAM) stays on the drive:
# the rootfs has ~21 MB free.
#
#   tools/build_3101_image.sh <Akai MPC-3.10.1-Gen1-update.img> <out dir> [sysroot for the az01-image check]
#
# Writes <out>/MPC-3.10.1-Gen1-update.img (+ .sha256). Needs python3, xz, e2fsprogs (debugfs, e2fsck); qemu-arm for
# the check with Akai's own az01-image (skipped without a sysroot).
set -eu
IMG=$1 OUT=$2 SYS=${3:-}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RT=$HERE/mpc-remote/runtime
AZ="python3 $HERE/nam-mpc/firmware/az01img.py"
W=$OUT/work3101
rm -rf "$W" && mkdir -p "$W"

$AZ info "$IMG"
$AZ extract "$IMG" "$W/part.xz"
xz -dc "$W/part.xz" > "$W/rootfs.ext4"
e2fsck -fn "$W/rootfs.ext4" >/dev/null

UNIT=/usr/lib/systemd/system/other-things-boot.service
WANT=/etc/systemd/system/multi-user.target.wants/other-things-boot.service
debugfs -w "$W/rootfs.ext4" -f - >/dev/null <<EOF
mkdir /usr/lib/other-things
write $RT/boot.sh /usr/lib/other-things/boot.sh
sif /usr/lib/other-things/boot.sh mode 0100755
sif /usr/lib/other-things/boot.sh uid 0
sif /usr/lib/other-things/boot.sh gid 0
write $RT/other-things-boot.service $UNIT
sif $UNIT mode 0100644
sif $UNIT uid 0
sif $UNIT gid 0
symlink $WANT $UNIT
EOF
e2fsck -fn "$W/rootfs.ext4" >/dev/null || { echo "filesystem check failed"; exit 1; }
# read everything back
debugfs -R "cat /usr/lib/other-things/boot.sh" "$W/rootfs.ext4" 2>/dev/null | cmp - "$RT/boot.sh"
debugfs -R "cat $UNIT" "$W/rootfs.ext4" 2>/dev/null | cmp - "$RT/other-things-boot.service"
debugfs -R "stat $WANT" "$W/rootfs.ext4" 2>/dev/null | grep -q "Fast link dest: \"$UNIT\"" || { echo "symlink missing"; exit 1; }
debugfs -R "stat /usr/lib/other-things/boot.sh" "$W/rootfs.ext4" 2>/dev/null | grep -q "Mode:  0755" || { echo "boot.sh mode"; exit 1; }

# the same xz settings as Akai's (CRC32 check, 8 MiB dictionary, 24 MiB blocks)
xz -6 -T0 --check=crc32 --block-size=25165824 -c "$W/rootfs.ext4" > "$W/new.xz"
xz -dc "$W/new.xz" | cmp - "$W/rootfs.ext4"
$AZ build "$IMG" "$W/new.xz" "$OUT/MPC-3.10.1-Gen1-update.img"
$AZ info "$OUT/MPC-3.10.1-Gen1-update.img" | grep -q "sha1 check   ok"
if [ -n "$SYS" ]; then
    qemu-arm -L "$SYS" "$SYS/usr/bin/az01-image" --verify "$OUT/MPC-3.10.1-Gen1-update.img"
fi
(cd "$OUT" && sha256sum MPC-3.10.1-Gen1-update.img > MPC-3.10.1-Gen1-update.img.sha256 && cat MPC-3.10.1-Gen1-update.img.sha256)
ls -l "$OUT/MPC-3.10.1-Gen1-update.img"
rm -rf "$W"
