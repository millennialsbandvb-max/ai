#!/bin/sh
# One-time MPC probe (started by mpc-probe.service after MPC). Once MPC's screen is up, it writes a report about how
# the screen, touchscreen and inMusic's built-in web server are set up, to mpc-probe/ on the SD card (or the internal
# drive if there's no SD card). Read-only: it only reads settings and takes screenshots. It runs once: delete the
# mpc-probe folder to run it again on the next boot. Wi-Fi passwords and web-server login data are never copied.
BIN=/usr/lib/nam-mpc/bin/mpc-probe

# where to write: the SD card if one is mounted, else the internal drive
DEST=""
if mountpoint -q /sdcard 2>/dev/null; then DEST=/sdcard
else
    for m in $(awk '$2 ~ "^/media/" && $2 !~ "az01-internal$" {print $2}' /proc/mounts); do DEST=$m; break; done
fi
[ -n "$DEST" ] || DEST=/media/az01-internal
OUT="$DEST/mpc-probe"
[ -f "$OUT/report.txt" ] && exit 0    # already done

i=0   # wait for MPC to be running (up to 5 minutes), then give it a minute to draw its screen
while ! pidof MPC >/dev/null && [ $i -lt 300 ]; do sleep 1; i=$((i + 1)); done
sleep 60
mkdir -p "$OUT" || exit 0
R="$OUT/report.txt.part"
: > "$R"

# lim <seconds> <command...>: run a command, stopping it if it takes longer (busybox has no timeout)
lim() {
    s=$1; shift
    "$@" & p=$!
    ( sleep "$s"; kill -9 "$p" 2>/dev/null ) & k=$!
    wait "$p" 2>/dev/null
    kill "$k" 2>/dev/null
}
sec() { printf '\n######## %s\n' "$*" >> "$R"; }
run() { sec "$*"; lim 20 "$@" >> "$R" 2>&1; }

sec "mpc-probe $(date)"
run uname -a
run cat /etc/os-release
run cat /proc/cmdline
run sh -c 'grep -E "model name|Hardware|Revision|processor" /proc/cpuinfo | sort | uniq -c'
run free
run cat /proc/mounts

sec "screen: sysfs"
for f in /sys/class/graphics/fb*/name /sys/class/graphics/fb*/virtual_size /sys/class/graphics/fb*/bits_per_pixel \
         /sys/class/graphics/fb*/stride /sys/class/graphics/fb*/modes /sys/class/drm/card*-*/status \
         /sys/class/drm/card*-*/enabled /sys/class/drm/card*-*/modes /sys/class/drm/card*/device/uevent; do
    [ -r "$f" ] && echo "$f: $(tr '\n' ' ' < "$f")" >> "$R"
done
run ls -l /dev/fb0 /dev/fb1 /dev/dri /dev/input /dev/uinput
run sh -c 'ls /sys/kernel/debug/dri/*/ 2>/dev/null && cat /sys/kernel/debug/dri/*/state 2>/dev/null | head -150'

sec "MPC process: what it has open and loaded"
pid=$(pidof MPC | awk '{print $1}')
if [ -n "$pid" ]; then
    echo "pid $pid: $(tr '\0' ' ' < /proc/$pid/cmdline)" >> "$R"
    ls -l /proc/$pid/fd 2>/dev/null | grep -E 'dri|fb[0-9]|input|uinput|mali|galcore|ion|dma' >> "$R"
    awk '{print $6}' /proc/$pid/maps 2>/dev/null | grep -E 'EGL|GLES|gbm|drm|mali|Mali|fb|wayland|X11' | sort -u >> "$R"
    tr '\0' '\n' < /proc/$pid/environ 2>/dev/null | grep -E '^(QT|EGL|DISPLAY|WAYLAND|JUCE|MALI|DRM|FB|XDG|SDL)' >> "$R"
    grep -E 'Threads|VmRSS' /proc/$pid/status >> "$R"
else
    echo "MPC not running" >> "$R"
fi

sec "screen + touch probe"
lim 30 "$BIN" "$OUT" >> "$R" 2>&1
run cat /proc/bus/input/devices

sec "uinput module"
run sh -c 'lsmod | grep -i uinput; modinfo uinput 2>&1 | head -5'
run modprobe uinput
run ls -l /dev/uinput

sec "network + web server"
run ip addr
run netstat -ltnp
run systemctl status az0x-webserver --no-pager
run sh -c 'journalctl -u az0x-webserver -n 60 --no-pager'
run sh -c 'ls -la /etc/az0x-webserver /data/az0x-webserver /tmp/az0x-webserver* 2>&1'
run cat /etc/az0x-webserver/az0x-webserver.conf
for port in $(netstat -ltnp 2>/dev/null | awk '/az0x-webserver/ {n = split($4, a, ":"); print a[n]}' | sort -u); do
    for path in / /index.html /files /legal /timedate /software-update /api/v1/subtree /api/v1/object-meta; do
        sec "GET http://127.0.0.1:$port$path"
        lim 10 wget -S -q -O - "http://127.0.0.1:$port$path" 2>&1 | head -c 20000 >> "$R"
    done
done
run sh -c 'ls /proc/*/fd/* -l 2>/dev/null | grep az0x-webserver | head'

sec "services + processes"
run sh -c 'systemctl list-units --type=service --state=running --no-pager | head -60'
run ps

sec "done $(date)"
mv "$R" "$OUT/report.txt"
sync
