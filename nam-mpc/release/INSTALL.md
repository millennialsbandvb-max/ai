# Installing NAM MPC

NAM MPC runs Neural Amp Modeler amp captures (`.nam`) and cab impulse responses (`.wav`) as an insert effect on
MPC OS standalone devices: MPC Live II, and other Gen1 units (Live, One, X, Force). It is **not** firmware:
it's a plugin for the VST2 host that's already inside MPC OS, and removing it is one script.

## What you need

- A **root shell (SSH)** on the MPC. Stock MPC OS doesn't provide one; the plugin route was worked out on modded
  units. Without it you can't install this.
- A 32-bit Gen1 device. `uname -m` should say `armv7l`.
- MPC OS with glibc @GLIBC@ or newer. The installer checks this for you.

## Install

From your computer:
```sh
scp -r NAM-MPC-@VERSION@ root@<mpc-ip>:/tmp/
ssh root@<mpc-ip>
sh /tmp/NAM-MPC-@VERSION@/install.sh
```
The installer:
1. stops MPC;
2. copies the plugin to `/sdcard/vst/`;
3. creates `/sdcard/vst/NAM/Models` and `/sdcard/vst/NAM/IRs`;
4. copies the touchscreen page to `/sdcard/Synths/`;
5. backs up `MPC.settings` and adds NAM MPC to its plugin list;
6. starts MPC again.

If it can't find `MPC.settings`, locate it (`find / -name MPC.settings 2>/dev/null`) and run
`SETTINGS=<that path> sh install.sh`.

## Add amps and cabs

```sh
scp "My Amp.nam" root@<mpc-ip>:/sdcard/vst/NAM/Models/
scp "My Cab.wav" root@<mpc-ip>:/sdcard/vst/NAM/IRs/
```
Sub-folders are fine. New files show up the next time you tap a Model or IR arrow, with no restart.
`/sdcard/NAM/Models` and `/sdcard/NAM/IRs` are read too.

**Pick small models.** The MPC's CPU is far weaker than a laptop's. Standard-size NAM captures most likely won't
run in real time. Look for **Nano**, **Feather** or **Lite** captures (on tone3000.com, filter by size). For a
**slimmable** model, turn the **Size** knob down until it plays cleanly.

Check a model before you use it in a project:
```sh
cd /tmp/NAM-MPC-@VERSION@ && ./nam-test /sdcard/vst/NAM-MPC.so "My Amp"
```
The last line gives its CPU cost. Under about 35% is comfortable; over 60% will likely crackle alongside a project.

## Use it

1. On an audio track with your guitar as the input, add **NAM MPC** as an insert effect.
2. Pick a model and an IR with the arrows or Q-Links, then set Input and Gate for your guitar.

The plugin processes the **left input**, so plug into input 1. The result goes to both sides.

| Control | What it does |
|---|---|
| Model < > | Previous or next `.nam` file |
| Normalize | Levels every model to the same loudness (when the model has loudness data) |
| Size | For slimmable models: lower = less CPU, less detail. No effect on other models |
| Input | Drive into the amp, -24 to +24 dB |
| Gate | Noise gate threshold; all the way down = Off |
| Bass, Middle, Treble | ±10 dB tone controls after the amp (5 = flat) |
| Output | Final level, -24 to +24 dB |
| Cab | Cab IR on or off |
| IR < > | Previous or next `.wav` cab IR. Only the first 23 ms is used, which covers a guitar cab |

Your model, IR and settings are saved with the project, by file name. If you rename or remove a file, the page shows
"Missing: <name>".

## Uninstall

```sh
sh /tmp/NAM-MPC-@VERSION@/uninstall.sh
```
This removes the plugin, its page and its settings entry. Your models and IRs stay in `/sdcard/vst/NAM`.

## If something goes wrong

- **NAM MPC isn't in the plugin list.** Check that the entry survived: `grep NAM-MPC <MPC.settings>`. If MPC
  rewrote the file, restore the backup the installer made next to it.
- **MPC crashes when you insert it.** Run `./nam-test /sdcard/vst/NAM-MPC.so` over SSH; it loads the plugin the same
  way and prints what fails. Please report the output.
- **Crackles.** The model is too heavy. Use a smaller model, lower Size, or bounce the track.
- **Silence.** The model shows "Error: …" (the file isn't a NAM model this version can read), or the input is on
  the right channel.

Editing `MPC.settings` and running a modded OS are at your own risk. The installer backs up your settings first.
