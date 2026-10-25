#!/bin/sh
# Install the MPC browser into /data/mpc-browser (started by the remote page's Install button, via mpc-remote).
# Downloads Chromium for 32-bit ARM and the libraries it needs from Debian (the list and their SHA-256 are in
# packages.txt), checks each one, unpacks and arranges them; or, if MPC-Browser.tar.gz is on a card, unpacks that.
# Progress goes to /tmp/mpc-browser-install for the page. Needs about 700 MB free on the internal storage, once.
B=${MPCB_BIN:-/usr/lib/nam-mpc/bin}   # (the overrides are for testing off the device)
D=${MPCB_DIR:-/data/mpc-browser}
N=$D.new
C=$D.cache
ST=${MPCB_STATUS:-/tmp/mpc-browser-install}
say() { echo "$*" > "$ST"; echo "$*"; }
fail() { say "install failed: $*"; rm -rf "$N"; exit 1; }

rm -rf "$N"
mkdir -p "$N" || fail "can't write to /data"
card=$(ls ${MPCB_CARDS:-/media/*/MPC-Browser.tar.gz /sdcard/MPC-Browser.tar.gz} 2>/dev/null | head -n 1)
if [ -n "$card" ]; then
    say "unpacking $card"
    tar -xzf "$card" -C "$N" || fail "couldn't unpack $card"
else
    mkdir -p "$C" "$N/root" || fail "can't write to /data"
    total=$(grep -vc '^#' "$B/browser-packages.txt")
    i=0
    grep -v '^#' "$B/browser-packages.txt" > "$N/list"
    while read -r f sum; do
        i=$((i + 1))
        say "downloading $i of $total: $(basename "$f")"
        base=https://deb.debian.org/debian
        case "$f" in pool/updates/*) base=https://deb.debian.org/debian-security ;; esac
        "$B/debfetch" get "$base/$f" "$sum" "$C/$(basename "$f")" || fail "download of $(basename "$f") (is the MPC online?)"
        "$B/debfetch" data "$C/$(basename "$f")" "$N/root" || fail "unpacking $(basename "$f")"
    done < "$N/list"
    say "arranging"
    R="$N/root"
    mkdir -p "$N/lib" "$N/share" "$N/home"
    mv "$R/usr/lib/chromium" "$N/chromium" || fail "no chromium in the packages"
    rm -f "$N"/chromium/libVk* "$N"/chromium/*.TOC "$N/chromium/vk_swiftshader_icd.json"
    find "$N/chromium/locales" -type f ! -name 'en-US*' -exec rm -f {} \;
    for d in "$R/usr/lib/arm-linux-gnueabihf" "$R/lib/arm-linux-gnueabihf"; do
        [ -d "$d" ] && cp -a "$d"/. "$N/lib/"
    done
    rm -rf "$N/lib/gdk-pixbuf-2.0" "$N/lib/gio" "$N/lib/glib-2.0"
    mv "$R/usr/share/fonts" "$N/share/fonts"
    cp "$B/browser-run.sh" "$N/run.sh" && cp "$B/browser-asoundrc" "$N/home/.asoundrc" || fail "missing files in the firmware"
    chmod 755 "$N/run.sh"
    rm -rf "$R" "$N/list"
fi
[ -x "$N/chromium/chromium" ] || fail "chromium missing after unpacking"
cp "$B/browser-packages.txt" "$N/packages.txt"   # what this install is, so a newer firmware can tell it's out of date
rm -rf "$D" && mv "$N" "$D" || fail "couldn't move it into place"
rm -rf "$C"
sync
say "installed"
