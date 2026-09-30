#!/bin/sh
# Remove the report folders the MPC probe (an earlier test firmware) left on the MPC's drives: mpc-probe, mpc-probe2
# and mpc-probe2.new, on the internal storage and every mounted card/drive. Nothing else is touched. It waits a
# couple of minutes first so cards and USB drives are mounted, and checks again at every boot (cheap: it only looks
# for those three folder names), so a card that wasn't in the first time is cleaned when it next is.
sleep 120
for d in /sdcard /data /media/*; do
    for n in mpc-probe mpc-probe2 mpc-probe2.new; do
        [ -d "$d/$n" ] && rm -rf "${d:?}/$n" && echo "removed $d/$n"
    done
done
sync
