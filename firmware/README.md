# PocketBeat firmware for M5StickS3

Wearable foot drum. The board does all the detection; settings live on the board.

## Flash it (no Arduino IDE needed)

1. Download `pocketbeat_sticks3.bin` from the latest **Actions → Build PocketBeat firmware** run (Artifacts section).
2. Plug the StickS3 into your PC with a USB-C cable. Hold the **side button** (reset/power) while plugging in if it doesn't show up.
3. In Chrome on the PC open https://espressif.github.io/esptool-js/ → **Connect** → pick the board's COM port.
4. Flash address `0x0`, choose the `.bin`, **Program**. Wait for "Leaving...", then press the board's reset button.

## Use it

- **Power on.** Screen shows BPM, last hit, Bluetooth and Wi-Fi status, battery.
- **Stomp = kick.** Snare is a swing to the right by default (see Set right), or a hard stomp if you change the mode.
- **Set right (once):** press the front button **A**, then within 2.5 s swing your leg out to the right. The screen changes from "SET RIGHT" to "snare: swing R". Saved on the board.
- **Hi-hat** starts after 3 even hits and runs until you stop for a full bar.
- **Hold A for 2 s** = reset all settings.

## Sound

- **Board speaker** (default): plays the sampled kit on its own. Small speaker — a practice monitor.
- **Bluetooth MIDI:** always on, advertises as "PocketBeat". In GarageBand: Settings (wrench) → Advanced → Bluetooth MIDI Devices → PocketBeat. Pick a drum kit track and play. Any BLE MIDI drum app works the same.
- **Phone via the tuning page:** choose "This phone" or "Both" on the page and tap "Enable sound on this phone".

## Tune it

Join the Wi-Fi network **PocketBeat** (no password) and open **http://192.168.4.1**. Live impact meter from the board, all thresholds and modes, kit choice, speaker volume. Everything you change is saved on the board immediately.

## Build from source

Arduino IDE 2.x, ESP32 board package 3.x, libraries: M5Unified, ESP Async WebServer (ESP32Async), Async TCP (ESP32Async), BLE-MIDI (lathoub), NimBLE-Arduino 1.4.x, MIDI Library.
Board: ESP32S3 Dev Module · Flash 8MB · Partition "8M with spiffs" · PSRAM "QSPI PSRAM" · USB CDC On Boot "Enabled".

`web/` holds the tuning page source; `web_assets.h` and `samples.h` are generated from it (see `make_samples_h.py` and the gzip step in the repo root).
