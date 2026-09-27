#!/bin/sh
# NAM MPC installer. Run it on the MPC as root, from the unzipped folder:   sh install.sh [-y]
# Stops MPC, copies the plugin to /sdcard/vst, the touchscreen page to /sdcard/Synths, backs up MPC.settings, adds
# NAM MPC to MPC's plugin list, and starts MPC again. Safe to run again to upgrade: your models and IRs are kept.
# If MPC.settings isn't found, pass its path: SETTINGS=/path/to/MPC.settings sh install.sh
set -e
cd "$(dirname "$0")"
SO_DIR=/sdcard/vst; SO=NAM-MPC.so; SKIN='NAM-MPC - VST - NAM MPC'; NEEDS_GLIBC=@GLIBC@
YES=0; [ "$1" = "-y" ] && YES=1
die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
case "$(uname -m)" in armv7*) ;; *) die "this build is for 32-bit ARM MPC OS devices (MPC Live II and other Gen1 units); this one is $(uname -m)" ;; esac
[ -f payload/vst/$SO ] || die "run this from the unzipped NAM-MPC folder"
if command -v sha256sum >/dev/null && ! sha256sum -c SHA256SUMS >/dev/null 2>&1; then die "files damaged (SHA256SUMS mismatch): copy the folder again"; fi
GLIBC=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')
[ -n "$GLIBC" ] || GLIBC=$(ldd --version 2>/dev/null | awk 'NR==1{print $NF}')
if [ -n "$GLIBC" ]; then
    [ "$(printf '%s\n%s\n' "$NEEDS_GLIBC" "$GLIBC" | sort -t. -k1,1n -k2,2n | head -n 1)" = "$NEEDS_GLIBC" ] ||
        die "this MPC has glibc $GLIBC but this build needs $NEEDS_GLIBC or newer: update MPC OS, or use the CI build"
else
    echo "warning: couldn't read this device's glibc version (the plugin needs $NEEDS_GLIBC or newer)"
fi
[ -n "$SETTINGS" ] || SETTINGS=$(ls /media/az01-internal/Settings/*/MPC.settings 2>/dev/null | head -n 1)
[ -n "$SETTINGS" ] || SETTINGS=$(find /media -maxdepth 5 -name MPC.settings 2>/dev/null | head -n 1)
[ -f "$SETTINGS" ] || die "MPC.settings not found; pass it as SETTINGS=/path/to/MPC.settings"
command -v systemctl >/dev/null || die "systemctl not found"
[ -d /sdcard ] || die "/sdcard not found"
grep -q '/sdcard/Synths' "$SETTINGS" || echo "warning: /sdcard/Synths isn't in MPC's SynthContentLocations; the touchscreen page may not show"

echo "Installing NAM MPC:"
echo "  $SO_DIR/$SO, $SO_DIR/NAM/ (models and IRs), /sdcard/Synths/$SKIN"
echo "  and an entry in $SETTINGS"
if [ $YES = 0 ]; then
    printf "MPC will be stopped and restarted. Save your project first. Continue? [y/N] "
    read -r ok; case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi

systemctl stop acvs
trap 'systemctl start acvs' EXIT
i=0; while pidof MPC >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
pidof MPC >/dev/null && die "MPC did not stop"

mkdir -p "$SO_DIR/NAM/Models" "$SO_DIR/NAM/IRs" /sdcard/Synths
cp payload/vst/$SO "$SO_DIR/$SO.new" && mv "$SO_DIR/$SO.new" "$SO_DIR/$SO"
cp -n payload/vst/NAM/Models/* "$SO_DIR/NAM/Models/" 2>/dev/null || true
cp -n payload/vst/NAM/IRs/* "$SO_DIR/NAM/IRs/" 2>/dev/null || true
rm -rf "/sdcard/Synths/$SKIN.new" && cp -a "payload/Synths/$SKIN" "/sdcard/Synths/$SKIN.new"
rm -rf "/sdcard/Synths/$SKIN" && mv "/sdcard/Synths/$SKIN.new" "/sdcard/Synths/$SKIN"

BAK="$SETTINGS.bak-nam-mpc-$(date +%Y%m%d-%H%M%S)"
cp "$SETTINGS" "$BAK"
awk -v mode=add -v file="$SO_DIR/$SO" -v entryfile=plugin.xml -f plugin_list.awk "$SETTINGS" > "$SETTINGS.new"
n=$(grep -c "file=\"$SO_DIR/$SO\"" "$SETTINGS.new" || true)
[ "$n" = 1 ] || { rm -f "$SETTINGS.new"; die "settings edit failed (found the entry $n times); MPC.settings unchanged"; }
if command -v python3 >/dev/null; then
    python3 -c 'import sys, xml.etree.ElementTree as E; E.parse(sys.argv[1])' "$SETTINGS.new" 2>/dev/null ||
        { rm -f "$SETTINGS.new"; die "the edited settings aren't valid XML; MPC.settings unchanged"; }
fi
mv "$SETTINGS.new" "$SETTINGS"
sync

echo "Done. Settings backup: $BAK"
echo "Put .nam models in $SO_DIR/NAM/Models and .wav cab IRs in $SO_DIR/NAM/IRs (no restart needed: tap an arrow)."
echo "Starting MPC. Add 'NAM MPC' as an insert effect on an audio track."
