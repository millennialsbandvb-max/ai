# HeadRush Prime — Firmware Teardown & Looper Project Plan

**Analyzed image:** `HeadRush Prime 5.2.0 Firmware Updater BETA 3 - Win.exe` (Windows updater, 221 MB)
**Goal:** replace the stock performance app with a custom Roland RC‑600‑style looper, reusing the stock OS, drivers, and hardware I/O.

> Scope note: this is homebrew/right‑to‑repair work on hardware you own. This document maps how the
> firmware is built and how a *replacement app* drops into the existing OS. It does **not** cover defeating
> signature enforcement. Whether the boot chain cryptographically *gates* unsigned images is flagged below
> as the one open question; if it does, the correct path is the hardware recovery/debug port on the board.

---

## 1. How the update package is structured

```
HeadRush Prime 5.2.0 ... .exe        Windows self‑extracting 7‑Zip stub (PE32)
  └─ overlay 7z archive
       ├─ FirmwareUpdater.exe         (the actual Windows flasher UI)
       ├─ Background.png, Config.json (updater branding/config)
       └─ Update.img                  225 MB  ← the real firmware
```

`Config.json` confirms the device name ("HeadRush Prime") and the on‑device update entry point:
*Global Settings → … → Firmware Update*, then USB to the computer. `allowUpdateOnBattery: true`,
`forceUpdate: false`.

### `Update.img` — inMusic "AZ0x" container

Magic bytes `AZ 30 78` ("AZ0x") at offset 0. It is a flat container with a header string table naming the
sections, followed by records. Each payload section carries a **SHA‑256 digest**; all five verified
byte‑for‑byte on extraction:

| Section        | Offset    | Stored size | Format            | SHA‑256 |
|----------------|-----------|-------------|-------------------|---------|
| `boot`         | 0x3f0     | 568 KB      | FDT/FIT (U‑Boot)  | ✅ match |
| `splash`       | 0x8e530   | 266 KB      | XZ                | ✅ match |
| `updatesplash` | 0xd0d00   | 266 KB      | XZ                | ✅ match |
| `kernel`       | 0x113560  | 5.8 MB      | XZ → ext4 → FIT   | ✅ match |
| `rootfs`       | 0x6e3790  | 208 MB      | XZ → ext4         | ✅ match |

The container also names the image internally: **"HG06 upgrade image"** / device id **HG06**. (`HG06` is the
HeadRush Prime platform codename; `HV01` is the internal audio card; `Evil` is the app codename.)

Decompressed:
- `kernel.raw` → ext4 fs containing `kernel.fit` (U‑Boot **FIT** image → *verified‑boot capable* format)
- `rootfs.raw` → **514 MB ext4**, volume label `root`, **not encrypted**, fully extractable with `debugfs`.

---

## 2. The operating system

| Property      | Value |
|---------------|-------|
| Distro        | **Buildroot 2024.02.6** |
| Init          | **systemd** |
| Kernel        | Linux **6.1** (ARM) |
| CPU / ABI     | **32‑bit ARM, hard‑float** (`arm-linux-gnueabihf`), Rockchip SoC |
| GPU           | **Mali** (`libmali.so.14.0`, r1p0 blob) |
| Rootfs mount  | read‑only base + **overlayfs** (`overlayfs-setup.service`); user data on `/data` |

Relevant systemd units: `evil.service` (the app), `az0x-webserver.service`, `edisksd.service`,
`touch-fw-update.service`, `overlayfs-setup.service`, plus NetworkManager, BlueZ, avahi, wpa_supplicant.

Storage model worth knowing for the looper: user content is **not** on the rootfs. `runevil` builds and
mounts a loopback **`usb.img`** at `/data/Evil/usb_mnt` (this is also what the unit exposes as a USB mass‑
storage drive to a computer). Rigs/Setlists/Blocks/**Loops** directories live there.

---

## 3. The application ("Evil")

- **Binary:** `/usr/Evil/Evil`, 52 MB, stripped ELF (ARM hard‑float PIE).
- **Toolkit:** **Qt 5** — links `Qt5Quick`, `Qt5Qml`, `Qt5QmlModels`, `Qt5Gui`, `Qt5Core`, `Qt5Svg`,
  `Qt5Sql`, `Qt5DBus`, `Qt5Concurrent`, `Qt5Xml`, plus the Qt Virtual Keyboard plugin. **UI is QML.**
- **Audio/codec libs:** `libasound` (ALSA), `libavcodec/avformat/avutil/swresample` (FFmpeg 58/56/3),
  `libFLAC.12`, `libopus.0`, `libsbc`.
- **Other:** `libcurl`, `libssl/libcrypto 3`, `libcryptopp.8`, `libsentry` (crash reporting),
  `libNetworkManagerQt`, `libBluezQt`, `libmtdev` (multitouch), `libaz0x-webserver-client`.
- **Launch:** `evil.service` → `/usr/Evil/Scripts/runevil` → `/usr/Evil/Scripts/evil &` (forking, PIDfile
  `/tmp/evil.pid`, `Restart=on-failure`). `runevil` also pins audio IRQ threads to real‑time priority
  (`chrt -f -p 99 …` on the I2S/SoC IRQs) and isolates them on a CPU core — useful detail to copy for
  low‑latency looping.

Directory `/usr/Evil/` holds: `Evil`, `Scripts/`, `Resources/`, `QtQuick.2/`, `Assignments/`, `Content/`,
`Updates/`, `ReleaseNotes/`, `Web/`.

Reported firmware version in `Scripts/firmware.json`: `"version": "00.07"`. Release notes present up to
`5.2.0_beta3.md`.

---

## 4. Audio path (what the looper must drive)

ALSA, via `/etc/asound.conf`:

- **Internal card = `HV01`** (kernel drivers `snd-soc-rockchip-i2s`, `snd-soc-rockchip-inmusic-*`,
  `snd-soc-tas5805m-inmusic`, `snd-soc-eta5805`). Named PCM `HG06_internal` is a `plug` over `hw:HV01`.
- The app opens the card as `hw:HV01` (and `hw:HV01,%d` for sub‑devices).
- A **USB Audio Class 2 gadget** (`UAC2Gadget`) is also defined — 6 channels, **`S32_LE`**, with `dmix`/
  `dsnoop` for simultaneous in/out, and a 48 kHz "UAC48converted" plug. So the native format is
  **32‑bit, 48 kHz, multichannel**.

For a looper this is ideal: standard ALSA capture + playback on `hw:HV01`, 48 kHz / S32_LE. We can open the
same device the stock app uses (stock app must be stopped first — it owns the card).

---

## 5. Controls & display (footswitches, expression pedal, touchscreen)

- **Touchscreen:** multitouch via `libmtdev` → Qt. 7" panel; QML renders through Mali/EGL.
- **Footswitches / expression pedal:** handled in QML (`footswitchPressed(index)`, `setFootSwitchUserText`,
  `expressionPage`, `mayHaveExpr`) bridged to C++ (`hardwareAssign.cppPage`). Physical mapping lives behind
  that C++ layer; the control‑assignment *model* is data‑driven via `/usr/Evil/Assignments/*.qml`
  (`DefaultDevice.qml`, `DefaultAssignment.qml`, and M‑Audio pass‑through variants).
- The app branches on `Evil.isHV01()` and a `productDependent({fourInch: …, other: …})` helper, i.e. one
  codebase spans several HeadRush models; Prime is the `HV01`/non‑four‑inch branch.

**Action item for phase 2:** confirm the exact `/dev/input/eventN` (or GPIO/key) source for the 12
footswitches and the ADC channel for the expression pedal. These are read below the QML layer; I can pin
them down by disassembling the `hardwareAssign` C++ or by reading `/proc/bus/input/devices` on the unit.

---

## 6. Boot & update verification — THE decision point

- `boot` section and `kernel.fit` are both **U‑Boot FIT** images. FIT is the format U‑Boot uses for
  **verified boot** (hashed/signed `images{}` + `configurations{}`).
- The rootfs integrity in the AZ0x container is **SHA‑256 only** (an integrity/corruption check, verified
  above). The app ships OpenSSL + Crypto++ and there is an `az0x` verification layer plus bundled public
  keys for the boot chain.

**Open question that determines the whole flashing strategy:**

> Does U‑Boot (in `boot`) **enforce** a signature on the FIT kernel / rootfs and refuse to boot anything not
> signed by inMusic, or is verification advisory / integrity‑only?

- **If integrity‑only:** repackage a modified rootfs (our looper app swapped in), recompute the SHA‑256s,
  rebuild the AZ0x container, and it installs/boots through the normal updater. Clean path, no hardware.
- **If signatures are enforced in U‑Boot:** a modified rootfs won't boot via the normal updater. The
  legitimate route is then the **board‑level recovery/debug path** (serial console / recovery DFU — note the
  `hg06_update_*.dfu` present in `Scripts/`), which is a hands‑on‑hardware matter, not a cryptographic one.

I can determine which case we're in by analyzing the U‑Boot config inside `boot` (no hardware required).
That should be the next technical step before writing any looper code.

---

## 7. Recommended architecture for the looper

**Do not write a new OS.** Keep Buildroot + kernel + audio drivers + input/display stack. Replace only the
app:

1. New **Qt 5 / QML** application built for `arm-linux-gnueabihf` against the same Qt the device ships, so it
   drops into the `evil.service` slot (or a sibling unit) and renders on the existing Mali/EGL stack.
2. Audio engine opens **`hw:HV01`, 48 kHz, S32_LE**, duplex, small period — reuse the stock RT‑priority IRQ
   pinning from `runevil`.
3. Input: subscribe to the same footswitch/expression/touch sources (phase‑2 mapping).
4. Store loops/sessions under the existing `/data/Evil/usb_mnt/Loops` convention so they show up over USB.

### RC‑600‑style feature target
- 6 stereo tracks: record / play / overdub / undo‑redo, per‑track level & pan.
- Master tempo; loop‑length quantize; sync all tracks; one‑shot / multiply modes.
- Rhythm (drum) track + tap tempo + count‑in.
- Per‑track and master input FX (reuse FFmpeg/DSP already on the box where sensible).
- Freely assignable footswitches; "memories" (saved full setups).
- Touchscreen mixer view.

---

## 8. Risks / ground rules

- I build; **you flash and test** on the unit, with iteration cycles.
- **Brick risk is real** — before flashing anything custom, confirm the Prime's forced‑update/recovery mode
  works with an *official* image, and keep a **stable (non‑beta)** official firmware as a fallback.
- Warranty void; at your own risk.
- RC‑600 *behavior* is reimplemented from scratch — no Roland code, assets, or UI.

---

## 9. Next steps (proposed order)

1. **Analyze U‑Boot in `boot`** → answer the signing question (§6). *Gates everything.*
2. **Pin down control I/O** (§5) → exact footswitch/pedal/touch devices.
3. **Set up the ARM/Qt cross‑build** matching the device's Qt version.
4. **Scaffold the looper** (audio engine first, headless‑testable, then QML UI).
5. **Repackage & flash loop** once §1 confirms the install path.
