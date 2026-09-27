#!/bin/sh
# NAM MPC boot helper (started by nam-mpc.service after MPC). If MPC.settings doesn't list NAM MPC, stop MPC, back
# the settings up, add the entry and start MPC again. It never touches anything else, and exits quietly on any problem.
# Log: /tmp/nam-mpc-boot.log
SO=/usr/lib/nam-mpc/NAM-MPC.so
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
if grep -q "file=\"$SO\"" "$SETTINGS"; then echo "already listed in $SETTINGS"; exit 0; fi

echo "adding NAM MPC to $SETTINGS"
systemctl stop acvs
i=0; while pidof MPC >/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
if pidof MPC >/dev/null; then echo "MPC did not stop; leaving settings alone"; systemctl start acvs; exit 0; fi

cp "$SETTINGS" "$SETTINGS.bak-nam-mpc"
if awk -v mode=add -v file="$SO" -v entryfile="$DIR/plugin.xml" -f "$DIR/plugin_list.awk" "$SETTINGS" > "$SETTINGS.new" &&
   [ "$(grep -c "file=\"$SO\"" "$SETTINGS.new")" = 1 ] && grep -q '</PROPERTIES>' "$SETTINGS.new"; then
    mv "$SETTINGS.new" "$SETTINGS"
    echo "added (backup: $SETTINGS.bak-nam-mpc)"
else
    rm -f "$SETTINGS.new"
    echo "edit failed; settings unchanged"
fi
sync
systemctl start acvs
