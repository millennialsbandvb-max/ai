# SFZ MPC

A VST2 instrument for the MPC Live II (MPC OS / Hakai, Gen1 ARM) that plays SFZ sample instruments with
[sfizz](https://sfz.tools/sfizz) (BSD-2-Clause, source: `sfizz-1.2.3.tar.gz` at the repo root).

## Instruments
Put SFZ instruments in a folder named `SFZ` at the top of the SD card (or any drive), one sub-folder each, e.g.

    SFZ/Weresax/Programs/*.sfz
    SFZ/Weresax/Samples/...

Every `.sfz` file found (up to 3 folders deep) shows in the instrument selector, except pieces other `.sfz` files
`#include`. The alto sax used for testing is Karoryfer's Weresax (CC0):
https://github.com/sfzinstruments/karoryfer.weresax

## Controls
Instrument (< > arrows), Volume (-24..+12 dB), Voices (8-64 polyphony), Quality (Normal/High/Best interpolation).
Projects remember the instrument by file name.

## Build
    ./build.sh host && ./build.sh arm && ./build.sh skin && ARM_SYSROOT=<MPC rootfs> ./build.sh bundle
    SFZ_MPC_DIR=<folder with .sfz> ARM_SYSROOT=<MPC rootfs> ./build.sh test
The bundle goes into a firmware image with `nam-mpc/firmware/patch_image.sh` (`EXTRA=`, merged with other bundles).
