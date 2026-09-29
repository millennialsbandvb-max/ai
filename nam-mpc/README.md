# NAM MPC

[Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore) as a native VST2 effect for MPC OS
standalone devices (MPC Live II and other 32-bit Gen1 units), loaded by the plugin host already inside MPC OS. It
follows the route and conventions of [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins), whose skin tools
draw its touchscreen page.

**Status:** it builds, and passes its offline tests on x86 (with AddressSanitizer) and on ARM under emulation. It
has **not** run on a real MPC yet. The first hardware test is what's needed next. See "Testing on the device".

Installing and using it: [release/INSTALL.md](release/INSTALL.md).

## Signal chain

Mono in (the left input), the same signal to both outputs:

input gain → NAM model (resampled to the model's rate when needed) → loudness normalize → noise gate →
bass/middle/treble → DC block → cab IR (up to 1024 taps) → output gain

- **Models and IRs load on a background thread.** The audio thread never reads a file or allocates. It picks up a
  ready model at the start of a block and hands the old one back to be freed.
- **Folders:** `<plugin dir>/NAM/{Models,IRs}` and `/sdcard/NAM/{Models,IRs}`, scanned one sub-folder deep and
  rescanned whenever an arrow is tapped. Set `NAM_MPC_DIR` to use another base folder (for tests).
- **Project state** is a text chunk: every parameter by key, plus the model and IR by file name.
- **The Model and IR steppers** show the file name as their value text. A tap on an entry selects it; a Q-Link turn
  steps one entry (the same rule mpc-vst-plugins' wrapper uses for option lists). The arrows are momentary and are
  reported back to 0 after each press.

## Parameters

VST indices are what the skin and saved projects bind to. Append new ones; never reorder.

| # | Key | Range |
|---|---|---|
| 0 | input | -24 to 24 dB |
| 1 | gate | -100 (Off) to -20 dB |
| 2–4 | bass, middle, treble | 0–10, 5 = flat, ±10 dB |
| 5 | output | -24 to 24 dB |
| 6 | model | the file list (display: name) |
| 7, 8 | model_prev, model_next | momentary |
| 9 | cab | Off/On |
| 10 | ir | the file list (display: name) |
| 11, 12 | ir_prev, ir_next | momentary |
| 13 | normalize | Off/On |
| 14 | size | 0–100 % (slimmable models) |

`params.json` and `layout.conf` describe the same list for the skin build. Keep all three in step.

## Building

```sh
./build.sh host      # x86 build with ASan + the offline test (needs g++, python3)
./build.sh           # ARM plugin + nam-test, the skin, and build/NAM-MPC-<version>.zip
```
The ARM build uses `arm-linux-gnueabihf-g++` (`apt install g++-arm-linux-gnueabihf`). CI
(`.github/workflows/build-nam.yml`) builds inside `arm32v7/gcc:12` instead, which needs glibc ≤ 2.36 on the device.
Ubuntu 24.04's cross compiler needs 2.38. Either way the plugin exports only `VSTPluginMain`, and it links
libstdc++ statically so it can't clash with MPC's own.

Sources come from the zips at the repo root, so builds don't fetch anything:
NeuralAmpModelerCore (MIT), AudioDSPTools (MIT; only its Lanczos resampler), Eigen (MPL-2.0), and mpc-vst-plugins
(build-time skin tools). Their licenses go into the release zip.

Run the ARM test under emulation with `qemu-arm -L /usr/arm-linux-gnueabihf build/nam-test build/NAM-MPC.so`,
setting `NAM_MPC_DIR=build/fixture`.

## Testing on the device

1. `./nam-test /sdcard/vst/NAM-MPC.so` over SSH, before and after installing. It must print PASSED; it also gives
   the real CPU cost.
2. Insert on an audio track, play through it, and try every control and Q-Link.
3. Save the project, reload it, and check that the model, IR and settings came back.
4. Record what worked, including the device, MPC OS version and CPU figures, so the notes can be updated.

## Known limits

- **CPU.** Standard-size models are unlikely to run in real time on Gen1 hardware. On an x86 server core a
  standard WaveNet costs about 13% of real time; a Cortex-A17 is several times slower. Use Nano/Feather/Lite
  models or the Size knob. `nam-test` measures it on the device.
- **Mono, left input only.**
- **Cab IRs are cut to 1024 taps** (23 ms) with a short fade, to keep direct convolution cheap.
- **Resampling latency.** Models trained at 48 kHz run through a resampler, which adds a little latency. MPC
  isn't told about it.

## Feather copies of bigger models (distillation)

`tools/distill.py` trains a Feather model to imitate a bigger one (Standard, Lite, or an A2/slimmable model at
full size): it runs the original over NAM's standard training signal (`input.wav`, v3) with NAM's C++ engine,
trains a Feather on that, and writes `F<original name>.nam` with the original's metadata and the measured ESR
against the original. It needs `torch` and `neural-amp-modeler` (the trainer imports `tkinter` only for GUI
pop-ups), and a `nam-render` binary built from NeuralAmpModelerCore's `tools/render.cpp`:

```sh
distill.py "My Amp.nam" input.wav out/ --epochs 100 --render ./nam-render
```
On a CPU, 100 epochs take about 2-3 hours per model.
