#!/bin/sh
# NAM MPC uninstaller. Run on the MPC as root:   sh uninstall.sh [-y]
# Removes the plugin, its touchscreen page and its MPC.settings entry (backing the settings up first).
# Your models and IRs in /sdcard/vst/NAM are kept; delete that folder yourself if you want them gone.
set -e
cd "$(dirname "$0")"
SO_DIR=/sdcard/vst; SO=NAM-MPC.so; SKIN='NAM-MPC - VST - NAM MPC'
YES=0; [ "$1" = "-y" ] && YES=1
die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" = 0 ] || die "run as root"
[ -n "$SETTINGS" ] || SETTINGS=$(ls /media/az01-internal/Settings/*/MPC.settings 2>/dev/null | head -n 1)
[ -n "$SETTINGS" ] || SETTINGS=$(find /media -maxdepth 5 -name MPC.settings 2>/dev/null | head -n 1)
[ -f "$SETTINGS" ] || die "MPC.settings not found; pass it as SETTINGS=/path/to/MPC.settings"
if [ $YES = 0 ]; then
    printf "Remove NAM MPC? MPC will be restarted; projects using it lose the effect. [y/N] "
    read -r ok; case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi
systemctl stop acvs
trap 'systemctl start acvs' EXIT
i=0; while pidof MPC >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
pidof MPC >/dev/null && die "MPC did not stop"
BAK="$SETTINGS.bak-nam-mpc-$(date +%Y%m%d-%H%M%S)"
cp "$SETTINGS" "$BAK"
awk -v mode=remove -v file="$SO_DIR/$SO" -f plugin_list.awk "$SETTINGS" > "$SETTINGS.new"
if grep -q "file=\"$SO_DIR/$SO\"" "$SETTINGS.new"; then rm -f "$SETTINGS.new"; die "couldn't remove the entry; MPC.settings unchanged"; fi
mv "$SETTINGS.new" "$SETTINGS"
rm -f "$SO_DIR/$SO"
rm -rf "/sdcard/Synths/$SKIN"
sync
echo "Removed. Settings backup: $BAK. Models and IRs kept in $SO_DIR/NAM."
