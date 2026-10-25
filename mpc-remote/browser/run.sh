#!/bin/sh
# The MPC browser (started and stopped by mpc-remote): Chromium, headless, at low priority. The remote page shows and
# drives it through its DevTools (127.0.0.1:9222, tunnelled by mpc-remote); its sound goes to Web In (home/.asoundrc).
D=/data/mpc-browser
cd "$D" || exit 1
mkdir -p home cache profile
cat > fonts.conf <<EOC
<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig><dir>$D/share/fonts</dir><cachedir>$D/cache</cachedir></fontconfig>
EOC
# Widevine (Netflix's copy protection): if this install lacks it (an older install, or one from a card), fetch it now
# (the firmware's package list has its address and fingerprint; about 4 MB)
B=/usr/lib/nam-mpc/bin
if [ ! -f chromium/WidevineCdm/manifest.json ] && [ -x "$B/debfetch" ]; then
  wv=$(grep -i '^https://.*widevine' "$B/browser-packages.txt" | head -n 1)
  if [ -n "$wv" ]; then
    set -- $wv
    rm -rf wv.tmp && mkdir -p wv.tmp/root &&
    "$B/debfetch" get "$1" "$2" wv.tmp/wv.deb && "$B/debfetch" data wv.tmp/wv.deb wv.tmp/root &&
    mkdir -p chromium/WidevineCdm/_platform_specific/linux_arm &&
    cp wv.tmp/root/opt/WidevineCdm/gmp-widevinecdm/latest/libwidevinecdm.so chromium/WidevineCdm/_platform_specific/linux_arm/ &&
    cp wv.tmp/root/opt/WidevineCdm/manifest.json chromium/WidevineCdm/ &&
    echo "run.sh: Widevine installed" || { echo "run.sh: Widevine download failed"; rm -f chromium/WidevineCdm/manifest.json; }
    rm -rf wv.tmp
  fi
fi
echo "run.sh: Widevine files: $(ls -l chromium/WidevineCdm/manifest.json chromium/WidevineCdm/_platform_specific/linux_arm/libwidevinecdm.so 2>&1 | tr '\n' ' ')"
# Widevine (if installed): tell Chromium where it is, as its component updater would (that's off)
if [ -f chromium/WidevineCdm/manifest.json ]; then
  mkdir -p profile/WidevineCdm
  echo "{\"Path\":\"$D/chromium/WidevineCdm\"}" > profile/WidevineCdm/latest-component-updated-widevine-cdm
  mkdir -p home/.local/lib
  ln -sfn "$D/chromium/WidevineCdm/_platform_specific/linux_arm/libwidevinecdm.so" home/.local/lib/libwidevinecdm.so
fi
export HOME="$D/home" FONTCONFIG_FILE="$D/fonts.conf" LD_LIBRARY_PATH="$D/lib:$D/lib/pulseaudio:$D/chromium"
export PULSE_SERVER=none   # no PulseAudio here: straight to ALSA
exec "$D/chromium/chromium" --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
  --remote-debugging-port=9222 --remote-allow-origins='*' --user-data-dir="$D/profile" --window-size=1240,544 \
  --autoplay-policy=user-gesture-required --renderer-process-limit=2 --disable-extensions --no-first-run \
  --disable-component-update --disable-background-networking --disable-features=Translate,MediaRouter \
  --user-agent='Mozilla/5.0 (X11; CrOS armv7l 15633.69.0) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/150.0.0.0 Safari/537.36' \
  --enable-logging=stderr --vmodule='*cdm*=1,*widevine*=1' --js-flags=--max-old-space-size=256 about:blank
