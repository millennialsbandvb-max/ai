# PocketBeat firmware for M5StickS3

Wearable foot drum. The board does all the detection; settings live on the board.

## Flash it (no Arduino IDE needed)

1. Download `pocketbeat_sticks3.bin` from the latest **Actions → Build PocketBeat firmware** run (Artifacts section).
2. Plug the StickS3 into your PC with a USB-C cable. Hold the **side button** (reset/power) while plugging in if it doesn't show up.
3. In Chrome on the PC open https://espressif.github.io/esptool-js/ → **Connect** → pick the board's COM port.
4. Flash address `0x0`, choose the `.bin`, **Program**. Wait for "Leaving...", then press the board's reset button.

## Using it with M5Launcher (several apps on one board)

Install Launcher once from https://bmorcelli.github.io/Launcher/ (pick M5StickS3; side button 2 s until the green LED blinks, then Connect). Launcher wipes the board, so PocketBeat has to be re-added through Launcher:

1. Power on. On Launcher's boot screen press the front button to open its menu.
2. Open **WebUI**: Launcher starts a Wi-Fi network (name and password shown on screen). Join it from your phone or PC and open the address it shows.
3. Upload `pocketbeat_sticks3.app.bin` (the app-only file, **not** the 8 MB one), then choose **Install** on it.
4. From then on the board boots straight into PocketBeat; hold the front button during the Launcher boot screen to get back to the menu and pick another app.

Only `pocketbeat_sticks3.app.bin` works inside Launcher. The plain `pocketbeat_sticks3.bin` is the whole-flash image for a board without Launcher.

## Use it

- **Power on.** Screen shows BPM, last hit, Bluetooth and Wi-Fi status, battery.
- **Default: stomp = kick, lift the foot high = snare** (gyro-based), hi-hat automatic from your tempo. Other modes on the tuning page: bounce = hat / stomp = kick / lift = snare, swing right for snare (see Set right), hard-stomp modes.
- **Set right (once):** press the front button **A**, then within 2.5 s swing your leg out to the right. The screen changes from "SET RIGHT" to "snare: swing R". Saved on the board.
- **Hi-hat** starts after 3 even hits and runs until you stop for a full bar.
- **Hold A for 1 s** = screen off (any button turns it back on). **Hold A for 6 s** = reset all settings.
- **Screen** turns off after 30 s without a button press to save battery; press any button to wake it (that press does nothing else).

## Sound

- **Board speaker** (default): plays the sampled kit on its own. Small speaker — a practice monitor.
- **USB MIDI:** plug the board into a PC/Mac (or an iPhone/iPad via a USB-C/camera adapter) and it appears as a MIDI input named "PocketBeat". Lowest latency.
- **Bluetooth MIDI:** always on, advertises as "PocketBeat". In GarageBand: Settings (wrench) → Advanced → Bluetooth MIDI Devices → PocketBeat. Pick a drum kit track and play. Any BLE MIDI drum app works the same.
- **Phone via the tuning page:** choose "This phone" or "Both" on the page and tap "Enable sound on this phone".

## Tune it

Join the Wi-Fi network **PocketBeat** (no password) and open **http://192.168.4.1**. Live impact meter from the board, all thresholds and modes, kit choice, speaker volume. Everything you change is saved on the board immediately.

## Build from source

Arduino IDE 2.x, ESP32 board package 3.x, libraries: M5Unified, ESP Async WebServer (ESP32Async), Async TCP (ESP32Async), BLE-MIDI (lathoub), NimBLE-Arduino 1.4.x, MIDI Library.
Board: ESP32S3 Dev Module · Flash 8MB · Partition "8M with spiffs" · PSRAM "QSPI PSRAM" · USB CDC On Boot "Enabled".

`web/` holds the tuning page source; `web_assets.h` and `samples.h` are generated from it (see `make_samples_h.py` and the gzip step in the repo root).

---

# Footboard (keyboard-only firmware)

Source: `footboard_sticks3/` (page: `footboard_web/index.html`, regenerate `fb_web.h` with `python3 make_footboard_web.py`).
Builds: `bin/footboard_sticks3.app.bin` (install from a tuning page upload, or later via Check for update) and `bin/footboard_sticks3.bin` (web flasher, address 0x0).

- Wi-Fi network **Footboard**, page at http://192.168.4.1. Bluetooth MIDI and USB MIDI are named "Footboard".
- Board flat on the foot. Swivel to pick a note, tap to play it, it holds until the foot tips away by the release angle.
- Harmony buttons m3, M3, 4, 5, 8 stack on top of the note (e.g. M3 + 5 = major triad).
- Front button: center. Side button: octave up. Hold front 1 s: screen off (any button wakes it). Hold front 6 s: reset settings (keeps Wi-Fi).
- Settings live in their own storage, separate from PocketBeat's, so switching between the two firmwares keeps both sets.

The drum firmware as of the split is saved in `saved/pocketbeat_drums_8546ce0/` (source included).

## Power (both firmwares)

- **Off:** hold the side button for 3 seconds ("Power off" shows, then it switches off).
- **Auto off:** after 10 minutes with no foot movement, no button press and no tuning page open.
- **On:** press the power button.
