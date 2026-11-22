#!/bin/sh
# Other Things add-on: entry point for MPC OS 3.10.1+'s autorun.sh ScriptRunner (the MPC asks before it runs).
# Put this file, the OtherThings folder and the VST2 Plugins folder at the top of a card or stick. If HAKAI's own
# files (the Hakai folder) are on the same drive, its launcher runs first (so this autorun.sh can replace HAKAI's).
# Nothing is installed: everything loads into memory (/run) and a reboot brings the MPC back to stock.
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOG=/tmp/other-things-autorun.log
exec >>"$LOG" 2>&1
echo "===== Other Things autorun $(date 2>/dev/null) from $ROOT ====="
DEPLOY="$ROOT/OtherThings/other-things-deploy.sh"
[ -r "$DEPLOY" ] || { echo "ERROR: missing $DEPLOY"; exit 1; }
/bin/sh "$DEPLOY" stage || exit 1          # our files into /run, our MPC launcher in place (before HAKAI restarts MPC)
if [ -r "$ROOT/Hakai/hakai-autorun-gen1.sh" ]; then
    echo "HAKAI found on this drive: its launcher first"
    HAKAI_AUTORUN_CONFIRMED=YES /bin/sh "$ROOT/Hakai/hakai-autorun-gen1.sh" || echo "HAKAI's launcher ended with an error"
fi
exec /bin/sh "$DEPLOY" start               # MPC restarted with ours if it isn't already, then the remote
