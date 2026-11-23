#!/bin/sh
# Other Things boot (in our MPC OS 3.10.1 image, run once per boot by other-things-boot.service): wait for MPC's
# first start and for the drive holding OtherThings/ ("MPC SSD" first), then run its start.sh: HAKAI's full mode
# and our add-on, with no question. Skipped if the drive has OtherThings/no-boot (then use autorun.sh by hand).
exec >>/tmp/other-things-boot.log 2>&1
echo "===== Other Things boot $(date 2>/dev/null) ====="
[ -e /run/other-things/booted ] && { echo "already ran this boot"; exit 0; }
i=0
while [ $i -lt 120 ] && ! systemctl is-active --quiet acvs.service; do sleep 1; i=$((i + 1)); done
ROOT=
i=0
while [ $i -lt 90 ] && [ -z "$ROOT" ]; do   # (up to 3 minutes for a slow drive to mount)
    for d in "/media/MPC SSD" /media/*; do
        [ -r "$d/OtherThings/start.sh" ] && { ROOT=$d; break; }
    done
    [ -n "$ROOT" ] || { sleep 2; i=$((i + 1)); }
done
[ -n "$ROOT" ] || { echo "no drive with OtherThings/start.sh: MPC stays stock"; exit 0; }
[ -e "$ROOT/OtherThings/no-boot" ] && { echo "$ROOT/OtherThings/no-boot: not starting"; exit 0; }
mkdir -p /run/other-things && : > /run/other-things/booted
sleep 5   # (let MPC finish its first start before HAKAI restarts it)
echo "starting from $ROOT"
HAKAI_AUTORUN_CONFIRMED=YES exec /bin/sh "$ROOT/OtherThings/start.sh"
