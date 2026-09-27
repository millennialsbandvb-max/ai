# NAM MPC firmware image (USB update)

`MPC-3.9.1-Gen1-update.img` (NAM build) is **Hakai 3.9.1 for Gen1 MPCs with NAM MPC added**. It installs from a USB
stick, the same way you installed Hakai, and you don't need SSH or a computer connection.

Only four things are added. A file-by-file comparison with the original Hakai image shows nothing else changed:

| Added | What it is |
|---|---|
| `/usr/lib/nam-mpc/` | the plugin, plus the small script and files that add it to MPC's plugin list |
| `/usr/share/Akai/Content/Synths/NAM-MPC - VST - NAM MPC/` | its touchscreen page, next to Akai's own |
| `/etc/systemd/system/nam-mpc.service` (+ enable link) | runs that script at start-up |

The start-up script only acts when NAM MPC is missing from `MPC.settings`, which is normally just the first boot. It
backs the settings up to `MPC.settings.bak-nam-mpc`, adds the one entry, and restarts MPC once. Its log is
`/tmp/nam-mpc-boot.log`.

## Before you start

- **It hasn't run on a real MPC yet.** It was checked against the real Hakai 3.9.1 contents:
  - the MPC program contains the VST2 plugin host;
  - glibc 2.39 is present (the plugin needs 2.38);
  - the settings folder is `/media/az01-internal/Settings/MPC`;
  - the plugin loads and processes audio with the firmware's own libraries (under emulation);
  - the settings edit works with the firmware's own BusyBox.

  Back up your projects and samples first anyway.
- **Keep the original Hakai `.img`.** Reinstalling it from USB removes NAM MPC and puts you back exactly where you
  were.
- **Gen1 MPCs only** (MPC Live II, Live, One, X): the same devices as the Hakai image it came from.

## Install

1. Copy the NAM build of `MPC-3.9.1-Gen1-update.img` to the top level of a USB stick. Remove any other `.img`
   files from it. The file name is the same as Hakai's, so keep the two in separate folders on your computer.
2. On the MPC, run the USB update the same way you installed Hakai.
3. After it restarts, MPC comes up, then quits and restarts once by itself. That's NAM MPC being added to the
   plugin list, and it happens only once.

If the MPC's update screen refuses the file because it's the same version you already have, tell me what it says.

## Add amps and cabs from a USB stick or SD card

Make this layout at the top level of a USB stick or SD card:
```
NAM/
  Models/   <- .nam files (sub-folders are fine)
  IRs/      <- .wav cab impulse responses
```
Plug it in, add **NAM MPC** as an insert effect on an audio track (guitar into input 1), and step through
models and IRs with the arrows. New files show up when you tap an arrow. Keep the drive plugged in while you
play: a project remembers the model and IR by file name.

**Pick small models: Nano, Feather or Lite.** Standard-size models are probably too heavy for the MPC's CPU.
For a slimmable model, turn **Size** down if you hear crackles.

## If NAM MPC isn't in the plugin list

Restart the MPC once. On a freshly reset MPC the settings file only exists after MPC has run, so the entry is added
on the next start-up. Hakai has SSH on, so on the same network you can also run `ssh root@<mpc-ip>` (empty
password) and read `/tmp/nam-mpc-boot.log`.

## Remove it

Reinstall the original Hakai image, or an official Akai update, from USB. The plugin-list entry stays in
`MPC.settings` pointing at a file that's gone, and MPC skips it.

## How it was built

```sh
nam-mpc/build.sh                                  # the plugin and its page
nam-mpc/firmware/patch_image.sh MPC-3.9.1-Gen1-update.img out/MPC-3.9.1-Gen1-update.img
```
`patch_image.sh` uses `mpcfit.py` to open the image. The image is a device tree holding one xz root filesystem and
its SHA-1. The script checks that the MPC program has the VST2 host, adds the files with `debugfs`, and runs
`e2fsck`. It then recompresses with the original's xz settings, writes the new SHA-1, and checks that the new image
unpacks to exactly the patched filesystem. `mpcfit.py selftest <image>` rebuilds an unmodified image and checks the
result is byte-identical to the original.
