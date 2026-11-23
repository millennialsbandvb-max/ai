#!/bin/sh
# Other Things add-on start (OtherThings/start.sh): run by the drive's autorun.sh (MPC OS 3.10.1+'s ScriptRunner
# asks first) or, with our 3.10.1 image, at every boot by other-things-boot.service (no question). The drive holds
# OtherThings/, VST2 Plugins/ and HAKAI's Hakai/ folder at its top; if Hakai/ is there, HAKAI's full mode loads first.
# Nothing is installed: everything loads into memory (/run); without our image a reboot brings the MPC back to stock.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
LOG=/tmp/other-things-autorun.log
exec >>"$LOG" 2>&1
echo "===== Other Things autorun $(date 2>/dev/null) from $ROOT ====="
DEPLOY="$ROOT/OtherThings/other-things-deploy.sh"
[ -r "$DEPLOY" ] || { echo "ERROR: missing $DEPLOY"; exit 1; }
/bin/sh "$DEPLOY" stage || exit 1          # our files into /run, our MPC launcher in place (before HAKAI restarts MPC)
if [ -r "$ROOT/Hakai/hakai-autorun-gen1.sh" ]; then
    # Straight into HAKAI's full mode (its launcher's Option 2), no launcher screen: HAKAI's own selector skips
    # its screen when the launcher app has left a choice (launcher-mode + launcher-mode.appchoice). To get the
    # launcher screen back, put a file named hakai-ask (any content) at the top of this drive.
    if [ ! -e "$ROOT/hakai-ask" ]; then
        mkdir -p /run/Hakai && printf '2\n' > /run/Hakai/launcher-mode && : > /run/Hakai/launcher-mode.appchoice
        echo "HAKAI found on this drive: its full mode (Option 2), no launcher screen"
    else
        echo "HAKAI found on this drive: its launcher (hakai-ask is on the drive)"
    fi
    HAKAI_AUTORUN_CONFIRMED=YES /bin/sh "$ROOT/Hakai/hakai-autorun-gen1.sh" || echo "HAKAI's launcher ended with an error"
fi
exec /bin/sh "$DEPLOY" start               # MPC restarted with ours if it isn't already, then the remote
