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

## 6. Boot & update verification — RESOLVED: full enforced verified boot

The open question is answered. The SoC is **Rockchip RK3288** (board `rk3288-az05-hg06`). The device runs a
**complete, enforced secure‑boot chain, and the rootfs is cryptographically locked with dm‑verity.** Every
stage is RSA‑2048 signed by the production key `headrush-1`:

```
BootROM
  → idbloader (TPL+SPL)                 ← in signed boot FIT
  → U-Boot                              ← signed; config signatures key "headrush-1" + "test-headrush"
  → kernel + board FDT + cmdline FIT    ← signed; config signature key "headrush-1"
  → rootfs = /dev/dm-0 (dm-verity, ro)  ← roothash baked into the SIGNED cmdline
```

**Evidence (all extracted from this image):**

1. **U‑Boot mandates signatures.** Its embedded control FDT contains:
   ```
   signature { key-headrush-1 { required = "conf"; algo = "sha256,rsa2048"; rsa,modulus = <256 bytes>; … } }
   ```
   `required = "conf"` means U‑Boot refuses to boot any configuration not signed by that key. The binary
   carries the matching enforcement paths: *"Failed to verify required signature"*, *"Unable to verify
   required signature"*, *"bad signature on secure image"*, *"boot update invalid: images failed to verify"*.
   Only the **production** key `headrush-1` is `required` (the `test-headrush` key is not in U‑Boot's
   required set).

2. **The kernel FIT is signed.** `conf-rk3288-az05-hg06` has `signature-0 { algo="sha256,rsa2048";
   key-name-hint="headrush-1"; sign-images="kernel","fdt" }`. Kernel, board device tree, and the kernel
   command line are all covered.

3. **The rootfs is dm‑verity, not just hash‑checked.** The *signed* `fdt-cmdline` sets:
   ```
   root=/dev/dm-0 ro  dm-mod.create="rootfs,,,ro,0 972800 verity 1 /dev/mmcblk0p10 /dev/mmcblk0p10 \
     1024 1024 486400 486401 sha256 521e802ae097ee9c33ce817c63d0f56aafa7b7e0832e3ecb01d0d3caaf5c2fea <salt>"
   ```
   dm‑verity builds a Merkle tree over all 486 400 × 1024‑byte (~475 MiB) data blocks; **any** modified
   block fails verification at read time and the mount fails. The root hash `521e802a…` lives inside the
   RSA‑2048‑signed cmdline, so it cannot be swapped for a hash of a modified rootfs either.

4. **The app cannot be overlaid.** Only `/etc` and `/var` are writable overlays
   (`/etc/az01/overlayfs.conf`, upper dir on `/data`). `/usr/Evil/Evil` sits inside the verity‑protected
   read‑only root and cannot be replaced without re‑signing the whole rootfs.

### What this means for the deliverable

A **PC updater `.exe` that installs a custom looper rootfs is not achievable through the normal update
path.** To make a modified image install *and boot*, you would need one of:

- **inMusic's private `headrush-1` RSA‑2048 key** — held only by inMusic; not in this image (only public
  keys ship). Not something I can produce or help obtain.
- **A secure‑boot / dm‑verity bypass** (boot‑chain exploit, or hardware glitching/key‑extraction on the
  board) — i.e. defeating the signature enforcement. Per the scope set at the start of this project, I will
  not develop that.

This is the boundary flagged in the original plan, and the device is on the strict side of it: not just
signed, but verity‑locked end to end. **See §7 for the paths that remain open.**

---

## 7. Paths that remain open

Given §6, the "swap the rootfs, ship a `.exe`" route is closed without crossing the security boundary.
What is still genuinely doable:

1. **Build the looper anyway, and prove it on a dev board.** The RK3288 + Qt5 + ALSA stack is standard and
   buyable (e.g. a Firefly/Rockchip RK3288 board, or any Linux SBC). I can build the full RC‑600‑style
   looper as a normal Linux/Qt app and have it running on commodity hardware. If inMusic ever ships a
   developer/unlock path for the Prime, it would then drop straight in. This is the most productive thing I
   can do without hardware‑side work on your unit.

2. **Headless/desktop prototype first.** The audio engine (tracks, sync, quantize, rhythm, overdub,
   undo) is hardware‑independent. I can build and test it on this machine, then on the SBC, so the design is
   proven before any device question is settled.

3. **Ask inMusic about developer access.** The presence of a `test-headrush` key hints there is an internal
   non‑production signing path. A direct request to inMusic for a developer‑unlocked unit or signing is the
   only *sanctioned* way onto the Prime itself.

4. **Use looper hardware that's open.** If the goal is mainly the RC‑600 workflow on a pedal, an open
   platform (e.g. a Raspberry‑Pi‑based looper, or the Daisy/Terrarium ecosystem) can run this same engine
   with no signing wall. I can target that instead.

What I will **not** do: extract/forge the signing key, or develop a secure‑boot / dm‑verity bypass for your
Prime. That's the line set when we started, and the firmware turned out to sit squarely on the far side of
it.

---

## 8. Looper architecture (for whichever target we pick)

Keep the OS/kernel/drivers; this is an **app**, not a new OS:

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

## 9. Risks / ground rules

- I build; **you flash and test** on the unit, with iteration cycles.
- **Brick risk is real** — before flashing anything custom, confirm the Prime's forced‑update/recovery mode
  works with an *official* image, and keep a **stable (non‑beta)** official firmware as a fallback.
- Warranty void; at your own risk.
- RC‑600 *behavior* is reimplemented from scratch — no Roland code, assets, or UI.

---

## 10. Next steps — NEEDS YOUR DECISION

§6 is resolved and it blocks the original "custom `.exe` for the Prime" deliverable. Pick a direction:

- **A. Build the looper for open hardware / a dev board** (recommended) — I start the audio engine now,
  test it headless here, then target an RK3288 SBC or an open looper platform. Produces a real, running
  looper; does not touch your Prime's security.
- **B. Stop here** — you keep this teardown as reference and decide later.
- **C. Pursue sanctioned Prime access** — you contact inMusic about developer unlock/signing; I resume the
  drop‑in app if that ever opens up.

I will not pursue a secure‑boot/verity bypass of the Prime.
