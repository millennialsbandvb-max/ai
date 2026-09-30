# Airwindows vocal set for MPC

Nine [Airwindows](https://www.airwindows.com/) plugins by Chris Johnson (MIT licence, `airwindows/LICENSE`), built as
VST2 effects for the MPC Live II (MPC OS / Hakai, Gen1 ARM), each with its own touchscreen page:

| Plugin | What it is | Controls |
|---|---|---|
| DeBess | de-esser (Listen: hear only what it's catching) | Intense, Sharp, Depth, Filter, Listen |
| VariMu | variable-mu tube compressor (the Fairchild/Manley type) | Intensity, Speed, Output, Dry/Wet |
| ButterComp2 | very smooth compressor | Compress, Output, Dry/Wet |
| Pressure6 | fast, clean compressor | Compress, Ratio |
| Air4 | high-end "air" | Air, Ground, Dark Freq, Ratio |
| Baxandall3 | bass + treble EQ | Input, Treble, Bass |
| Tube2 | tube saturation | Input, Tube |
| Doublelay | doubler | Detune, Delay L, Delay R, Feedback, Dry/Wet |
| Verbity2 | reverb | Room Size, Sustain, Mulch, Wetness |

All controls are 0-1 as in Airwindows' own plugins (0.5 is often "neutral").

## How it's built
`airwindows/<Plugin>/` are Airwindows' own sources (plugins/LinuxVST/src), unchanged. They're written against
Steinberg's VST2 SDK, which isn't redistributable, so `shim/` stands in for the part they use: the AudioEffectX
base class, over nam-mpc's hand-written VST2 ABI header. The shim also guards against unknown parameter indexes,
pads short saved states, and frees saved-state memory the plugins would leak.

    ./build.sh host && ./build.sh arm && ./build.sh params && ./build.sh skin && ARM_SYSROOT=<MPC rootfs> ./build.sh bundle

The bundle goes into a firmware image with `nam-mpc/firmware/patch_image.sh` (`EXTRA=`, merged with other bundles).
To add another Airwindows plugin: copy its LinuxVST source folder into `airwindows/`, add it to PLUGINS in build.sh
and pages.py.
