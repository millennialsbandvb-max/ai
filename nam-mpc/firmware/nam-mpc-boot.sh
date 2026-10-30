#!/bin/sh
# NAM MPC boot helper (started by nam-mpc.service after MPC). Every plugin built into the firmware has an entry in
# /usr/lib/nam-mpc/entries/*.xml (NAM MPC, the Dragonfly reverbs, ...). If MPC.settings is missing any of them:
# stop MPC, back the settings up, add the missing entries (all at once, so MPC restarts only once) and start MPC
# again. An entry already there is left alone unless its maker (manufacturer=, the folder MPC lists it under) or its
# name (name=, what MPC shows; e.g. Quad Rig became 0Quad Rig to list first) has changed, when it's replaced; nothing else is touched, and any problem leaves the settings as they were.
# Log: /tmp/nam-mpc-boot.log
DIR=/usr/lib/nam-mpc
exec >>/tmp/nam-mpc-boot.log 2>&1
echo "== $(date) nam-mpc boot"

i=0   # MPC.settings may appear only once MPC has run (a brand-new device); wait up to two minutes for it
while :; do
    SETTINGS=$(ls /media/az01-internal/Settings/*/MPC.settings 2>/dev/null | head -n 1)
    [ -n "$SETTINGS" ] && break
    i=$((i + 1))
    [ $i -ge 120 ] && { echo "MPC.settings not found; will try again next boot"; exit 0; }
    sleep 1
done

file_of() { sed -n 's/.* file="\([^"]*\)".*/\1/p' "$1"; }
entry_of() {   # the whole <PLUGIN .../> element listing file $1 (JUCE may wrap one over several lines), on one line
    awk -v ours="file=\"$1\"" '/<PLUGIN([ \t]|$)/ { e = "" ; on = 1 } on { e = e " " $0 } on && /\/>/ { on = 0; if (index(e, ours)) print e }' "$SETTINGS"
}
missing=""
for e in "$DIR"/entries/*.xml; do
    [ -f "$e" ] || continue
    f=$(file_of "$e")
    [ -n "$f" ] || { echo "skipping $e: no file= in it"; continue; }
    m=$(sed -n 's/.* manufacturer="\([^"]*\)".*/\1/p' "$e")
    n=$(sed -n 's/.* name="\([^"]*\)".*/\1/p' "$e")
    if ! grep -qF "file=\"$f\"" "$SETTINGS"; then missing="$missing $e"
    elif ! entry_of "$f" | grep -qF "manufacturer=\"$m\""; then echo "new maker ($m): $f"; missing="$missing $e"
    elif [ -n "$n" ] && ! entry_of "$f" | grep -qF " name=\"$n\""; then echo "new name ($n): $f"; missing="$missing $e"
    else echo "listed: $f"; fi
done
[ -n "$missing" ] || { echo "all plugins already listed in $SETTINGS"; exit 0; }   # (a changed one counts as missing)

echo "adding to $SETTINGS:$missing"
systemctl stop acvs
i=0; while pidof MPC >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
if pidof MPC >/dev/null; then echo "MPC did not stop; leaving settings alone"; systemctl start acvs; exit 0; fi

cp "$SETTINGS" "$SETTINGS.bak-nam-mpc"
cp "$SETTINGS" "$SETTINGS.work"
ok=1
for e in $missing; do   # entry file names have no spaces (they're ours)
    f=$(file_of "$e")
    if awk -v mode=add -v file="$f" -v entryfile="$e" -f "$DIR/plugin_list.awk" "$SETTINGS.work" > "$SETTINGS.new" &&
       [ "$(grep -cF "file=\"$f\"" "$SETTINGS.new")" = 1 ] && grep -q '</PROPERTIES>' "$SETTINGS.new"; then
        mv "$SETTINGS.new" "$SETTINGS.work"
    else
        echo "edit failed for $f"
        ok=0
        break
    fi
done
if [ $ok = 1 ]; then
    mv "$SETTINGS.work" "$SETTINGS"
    echo "added (backup: $SETTINGS.bak-nam-mpc)"
else
    rm -f "$SETTINGS.work" "$SETTINGS.new"
    echo "settings unchanged"
fi
sync
systemctl start acvs
