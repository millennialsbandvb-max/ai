# MPC Remote

See and touch the MPC Live II's screen from a web browser on the same Wi-Fi: `http://<MPC address>:8080/`.

- `server/`: the remote. One small C program (`mpc-remote.c`, no dependencies) with the page (`index.html`) built in.
  - Screen: MPC OS draws in software into a 32-bit DRM dumb buffer, 800x1280 (the panel is portrait, mounted
    sideways). The program reads the buffer on screen, sends changed 32x32 tiles as 16-bit colour over a WebSocket,
    and the page turns the picture upright. It only captures while someone is watching, at low priority.
  - Touch: clicks and drags are written into the touchscreen's own input device, so MPC sees a finger. The page's
    "Calibrate touch" asks for three taps on the real screen (MPC doesn't see those) to learn how the touch axes
    line up; the result is kept in `/data/mpc-remote/touch.conf`.
  - No password: anyone on the same network can open it.
  - `./build.sh` makes `build/bundle` (for `nam-mpc/firmware/patch_image.sh`, `EXTRA=`) and `build/mpc-remote-test`,
    a PC build that serves a BMP instead of the screen (`MPC_REMOTE_FAKE_BMP=...`) and prints touches.
  - `mpc-cleanup.sh`: removes the report folders the probe firmware left on the drives.
- `probe/`: the read-only probe used to find all this out (not in the firmware any more).
