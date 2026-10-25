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
export HOME="$D/home" FONTCONFIG_FILE="$D/fonts.conf" LD_LIBRARY_PATH="$D/lib:$D/lib/pulseaudio:$D/chromium"
export PULSE_SERVER=none   # no PulseAudio here: straight to ALSA
exec "$D/chromium/chromium" --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
  --remote-debugging-port=9222 --remote-allow-origins='*' --user-data-dir="$D/profile" --window-size=960,540 \
  --autoplay-policy=no-user-gesture-required --renderer-process-limit=2 --disable-extensions --no-first-run \
  --disable-component-update --disable-background-networking --disable-features=Translate,MediaRouter \
  --js-flags=--max-old-space-size=256 about:blank
