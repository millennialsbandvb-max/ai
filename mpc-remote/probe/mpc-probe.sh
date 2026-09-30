#!/bin/sh
# MPC probe (started by mpc-probe.service after MPC). Once MPC's screen is up, it writes a report about how the
# screen, touchscreen and inMusic's built-in web server are set up, as an mpc-probe2 folder on EVERY drive the MPC
# has mounted (the SD card, the internal drive, ...), so it can be found wherever the card shows up. It also copies
# in any report the first version (mpc-probe) left somewhere. Read-only apart from writing those folders: it only
# reads settings and takes screenshots. Wi-Fi passwords and web-server login data are never copied.
# It runs again on a boot where any drive is missing mpc-probe2/report.txt (e.g. after deleting it from the card).
BIN=/usr/lib/nam-mpc/bin/mpc-probe
NAME=mpc-probe2
TMP=/tmp/$NAME

# drives: every mount point under /media and /sdcard, except Akai's own content area; one per actual folder
# (the same drive can be mounted twice, e.g. /sdcard and /media/az01-internal-sd)
drives() {
    seen=""
    for m in $(awk '$2 ~ "^/media/" || $2 == "/sdcard" {print $2}' /proc/mounts); do
        case "$m" in /media/acvs-content*) continue ;; esac
        id=$(stat -c '%d:%i' "$m" 2>/dev/null) || continue
        case " $seen " in *" $id "*) continue ;; esac
        seen="$seen $id"
        echo "$m"
    done
}

i=0   # wait for MPC to be running (up to 5 minutes), then give it a minute to draw its screen and mount the card
while ! pidof MPC >/dev/null && [ $i -lt 300 ]; do sleep 1; i=$((i + 1)); done
sleep 60

todo=0
for d in $(drives); do [ -f "$d/$NAME/report.txt" ] || todo=1; done
[ $todo = 1 ] || exit 0   # every drive already has a report

rm -rf "$TMP"
mkdir -p "$TMP" || exit 0
R="$TMP/report.txt"
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
lim 30 "$BIN" "$TMP" >> "$R" 2>&1
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

sec "drives"
for d in $(awk '$2 ~ "^/media/" || $2 == "/sdcard" {print $1, $2, $3}' /proc/mounts | tr ' ' '|'); do
    echo "mount: $d" | tr '|' ' ' >> "$R"
done
echo "writing to: $(drives | tr '\n' ' ')" >> "$R"
run sh -c 'ls -la /media /sdcard /media/* 2>&1 | head -150'
run df

# the first version's report, wherever it went
seen=""
for d in /sdcard /media/*; do
    [ -d "$d/mpc-probe" ] || continue
    id=$(stat -c '%d:%i' "$d/mpc-probe" 2>/dev/null)
    case " $seen " in *" $id "*) continue ;; esac   # the same drive under another name
    seen="$seen $id"
    echo "first version's report found in $d/mpc-probe" >> "$R"
    t="$TMP/first-version$(echo "$d" | tr '/' '-')"
    mkdir -p "$t" && cp -r "$d/mpc-probe/." "$t/" 2>>"$R"
done

sec "done $(date)"
for d in $(drives); do
    rm -rf "$d/$NAME.new" && mkdir -p "$d/$NAME.new" && cp -r "$TMP/." "$d/$NAME.new/" &&
        rm -rf "$d/$NAME" && mv "$d/$NAME.new" "$d/$NAME" || rm -rf "$d/$NAME.new"
done
rm -rf "$TMP"
sync
