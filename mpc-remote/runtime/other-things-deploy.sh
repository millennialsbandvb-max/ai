#!/bin/sh
# Other Things add-on for MPC OS 3.10.1+ (run by autorun.sh): loads our improvements into memory, never into the
# MPC's own files (a reboot is stock again).   other-things-deploy.sh stage | start
#   stage: copies our programs from the drive (cards are mounted noexec) to /run/other-things/bin and puts our MPC
#          launcher in acvs.service through a runtime drop-in (it adds mpc-buttons.so: the remote's MPC buttons,
#          Web In, the browser on the MPC's screen, MIDI from your controllers to our effects, shortcuts)
#   start: restarts MPC if it isn't running with mpc-buttons.so yet, then starts the browser remote (port 8080)
# Test hooks: OT_RUN (default /run/other-things), OT_DROPIN (default /run/systemd/system/acvs.service.d).
set -u
SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN=${OT_RUN:-/run/other-things}
DROPIN=${OT_DROPIN:-/run/systemd/system/acvs.service.d}
LOG=$RUN/deploy.log
mkdir -p "$RUN/bin"
log() { echo "$(date '+%H:%M:%S' 2>/dev/null) $*" | tee -a "$LOG"; }
mpc_has_ours() {   # is MPC running with our library loaded? (MPC renames its process: look at every one's libraries)
    grep -l 'other-things/bin/mpc-buttons.so' /proc/[0-9]*/maps >/dev/null 2>&1
}
case "${1:-}" in
stage)
    for f in "$SELF"/bin/*; do
        [ -f "$f" ] || continue
        n=$(basename "$f")
        if ! cmp -s "$f" "$RUN/bin/$n" 2>/dev/null; then   # (mpc-buttons.so may be in use: replace, never overwrite)
            cp -f "$f" "$RUN/bin/$n.tmp" && mv -f "$RUN/bin/$n.tmp" "$RUN/bin/$n" || { log "ERROR copying $n (is /run full?)"; exit 1; }
        fi
        chmod 0755 "$RUN/bin/$n"
    done
    mkdir -p "$DROPIN"
    printf '[Service]\nExecStart=\nExecStart=/bin/sh %s/bin/mpc-launch\n' "$RUN" > "$DROPIN/zz-other-things.conf.tmp" &&
        mv -f "$DROPIN/zz-other-things.conf.tmp" "$DROPIN/zz-other-things.conf"
    systemctl daemon-reload
    log "staged from $SELF; MPC launcher in place"
    ;;
start)
    if mpc_has_ours; then
        log "MPC already runs with our library"
    elif [ -e /data/mpc-remote/no-buttons ]; then
        log "Button input off (the remote page): MPC left as it is"
    else
        log "restarting MPC with our library"
        systemctl restart acvs.service
        i=0
        while [ $i -lt 120 ] && ! mpc_has_ours; do sleep 1; i=$((i + 1)); done
        mpc_has_ours && log "MPC is up with our library after ${i}s" || log "WARNING MPC came up without our library (see /tmp/mpc-buttons.log)"
    fi
    systemctl stop other-things-remote.service >/dev/null 2>&1
    systemctl reset-failed other-things-remote.service >/dev/null 2>&1
    systemd-run --quiet --collect --no-block --unit=other-things-remote --nice=10 \
        --property=Restart=on-failure --property=RestartSec=5 "$RUN/bin/mpc-remote" 8080 &&
        log "browser remote started (http://mpc.local:8080)" || log "ERROR starting the browser remote"
    cp -f "$LOG" "$SELF/last-run.log" 2>/dev/null   # (a copy on the drive, to read on a computer)
    ;;
*) echo "usage: $0 stage | start" >&2; exit 2 ;;
esac
