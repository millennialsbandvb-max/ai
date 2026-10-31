/* Ambient Lab: eleven "machines" for strange ambient sound, for MPC OS (VST2, stereo). Each is our own take on the
 * kind of effect a well-known ambient pedal makes (near-miss names, personal use): a granular looper with 44
 * effects in 11 kinds x A-D (Mikrocosm), a wet channel beside an always-listening micro-looper (Moodd MK2), a
 * granular delay with eight modes (Partikle), a tape time-stretcher (Tensr), a sequenced-filter shimmer reverb
 * (NiteSky), a swelling ensemble reverb (Cloudbrst), cascading modulated delays (Polym00n), a three-voice pitched
 * rhythmic delay, free or in key (Hedr4), a swarm of short delays (Aftrneath), a warbling, badly-tracking pitch
 * shifter (Rainbow Mchn) and a square-wave synth that follows your notes with subharmonics (Data Korrupt). A
 * machine has eight knobs of its own (names, ranges: MACHINES[]) and 15 presets (PRESETS[]: Polym00n's and Hedr4's
 * are named after the pedals' factory presets, Mikrocosm's are its effects); Hold freezes what it has.
 * Building blocks (lines, filters, FDN reverb, pitch tracker) from vocal-mpc/src/vocal_core.h. Our own code.
 * Safety, as in every plugin here: input cleaned, reset if the output goes bad or past +36 dB, output capped at
 * +12 dB (one NaN reaching MPC's mixer silences it until restart). */
#include "vocal_core.h"   /* (and tools/picklist.h) */

#define NK 8
/* ==== knob meanings (as in Duo FX) ========================================================================= */
enum { M_LIN, M_EXP, M_INT, M_CHOICE };
typedef struct { const char *name; int map; float lo, hi, def; const char *unit; const char *const *choices; } Knob;
#define KLIN(n, lo, hi, d, u) {n, M_LIN, lo, hi, d, u, NULL}
#define KEXP(n, lo, hi, d, u) {n, M_EXP, lo, hi, d, u, NULL}
#define KINT(n, lo, hi, d, u) {n, M_INT, lo, hi, d, u, NULL}
#define KCHO(n, list, d) {n, M_CHOICE, 0, (float)(sizeof list / sizeof *list - 1), d, "", list}
#define KPCT(n, d) KLIN(n, 0, 100, d, "%")
#define KMS(n, lo, hi, d) KEXP(n, lo, hi, d, "ms")
#define KHZ(n, lo, hi, d) KEXP(n, lo, hi, d, "Hz")
#define KST(n, d) KINT(n, -24, 24, d, "st")
#define KNONE {NULL, 0, 0, 0, 0, NULL, NULL}

/* Mikrocosm's 44 effects: 11 kinds x variations A-D (after the Microcosm's algorithms) */
#define FX4(n) n " A", n " B", n " C", n " D"
static const char *const MC_FX[] = {FX4("Mosaic"), FX4("Seq"), FX4("Glide"), FX4("Haze"), FX4("Tunnel"), FX4("Strum"),
                                    FX4("Blocks"), FX4("Interrupt"), FX4("Arp"), FX4("Pattern"), FX4("Warp")};
enum { F_MOSAIC, F_SEQ, F_GLIDE, F_HAZE, F_TUNNEL, F_STRUM, F_BLOCKS, F_INTERRUPT, F_ARP, F_PATTERN, F_WARP };
static const char *const MOOD_WET[] = {"Reverb", "Delay", "Slip"};
static const char *const MOOD_LOOP[] = {"Env", "Tape", "Stretch"};
static const float MOOD_SPEED_V[] = {-2, -1, -0.5f, 0.5f, 1, 2};
static const char *const PT_MODE[] = {"Random", "Density", "LFO", "Rnd Pitch", "Reverse", "Detune", "Dns Pitch", "LFO Pitch"};
static const char *const DIRS[] = {"Fwd", "Rev", "Alt"};
static const char *const HD_SCALE[] = {"Off", "Major", "Minor", "Dorian", "Mixolydian", "Harm Minor", "Penta Maj", "Penta Min", "Blues", "Chromatic"};
static const char *const SHIM_PITCH[] = {"+12", "+7", "+5", "+19", "-12"};
static const float SHIM_ST[] = {12, 7, 5, 19, -12};
static const char *const RHYTHMS[] = {"Even", "Triplet", "Dotted", "Swing", "Cascade"};
static const char *const SECOND[] = {"Off", "Oct Down", "Unison", "Fifth", "Oct Up"};
static const float SECOND_ST[] = {0, -12, 0, 7, 12};
static const char *const SUBS[] = {"/2", "/3", "/4", "/5"};

enum { MK_MICRO, MK_MOOD, MK_PARTICLE, MK_TENSOR, MK_NIGHT, MK_CLOUD, MK_POLY, MK_HEDRA, MK_AFTER, MK_RAINBOW, MK_DATA, kNumMachines };
typedef struct { const char *name; float mix; const char *about; Knob k[NK]; } Machine;
static const Machine MACHINES[kNumMachines] = {
    [MK_MICRO] = {"Mikrocosm", 50, "Granular looper: 44 effects (11 kinds x A-D) of loops, grains, glitches, arps",
        {KCHO("Effect", MC_FX, 0), KPCT("Activity", 50), KPCT("Shape", 50), KEXP("Time", 0.05f, 2, 0.5f, "s"),
         KPCT("Repeats", 25), KHZ("Filter", 200, 16000, 12000), KPCT("Space", 35), KPCT("Reso", 10)}},
    [MK_MOOD] = {"Moodd MK2", 50, "Wet channel (reverb, delay, slip) beside an always-listening micro-looper",
        {KCHO("Wet", MOOD_WET, 0), KPCT("Time", 50), KPCT("Modify", 40), KCHO("Loop", MOOD_LOOP, 1),
         KEXP("Length", 0.25f, 8, 1.5f, "s"), KPCT("Loop Mod", 70), KPCT("Clock", 100), KPCT("Balance", 50)}},
    [MK_PARTICLE] = {"Partikle", 45, "Granular delay: eight ways to scatter, pitch and reverse grains",
        {KMS("Delay", 20, 2000, 400), KMS("Grain", 10, 300, 80), KEXP("Density", 1, 40, 12, "/s"), KST("Pitch", 0),
         KCHO("Mode", PT_MODE, 0), KPCT("Spread", 30), KPCT("Feedback", 30), KPCT("Tone", 60)}},
    [MK_TENSOR] = {"Tensr", 60, "Tape time: varispeed, reverse, stretch, stutter",
        {KLIN("Speed", -2, 2, 1, "x"), KLIN("Stretch", 0.25f, 1, 1, "x"), KHZ("Stutter", 0.5f, 16, 4), KPCT("Chance", 0),
         KEXP("Length", 0.2f, 4, 1, "s"), KCHO("Direction", DIRS, 0), KST("Pitch", 0), KPCT("Tone", 60)}},
    [MK_NIGHT] = {"NiteSky", 45, "Shimmer reverb through a sequenced, resonant filter",
        {KEXP("Decay", 1, 30, 6, "s"), KPCT("Size", 70), KPCT("Shimmer", 30), KCHO("Pitch", SHIM_PITCH, 0),
         KHZ("Filter", 200, 8000, 1500), KPCT("Reso", 40), KPCT("Seq Depth", 50), KHZ("Seq Rate", 0.1f, 8, 1)}},
    [MK_CLOUD] = {"Cloudbrst", 45, "Huge ensemble reverb that swells into pads",
        {KEXP("Decay", 1, 60, 12, "s"), KPCT("Size", 80), KPCT("Ensemble", 50), KPCT("Swell", 30), KMS("Predelay", 1, 500, 30),
         KPCT("Tone", 55), KLIN("Low", -12, 6, 0, "dB"), KPCT("Mod", 40)}},
    [MK_POLY] = {"Polym00n", 45, "Cascading modulated delays, phaser and flanger in repeats",
        {KMS("Time", 20, 1500, 380), KPCT("Feedback", 45), KINT("Dimension", 1, 6, 3, ""), KPCT("Depth", 40), KHZ("Rate", 0.05f, 8, 0.6f),
         KPCT("Phaser", 40), KPCT("Flanger", 30), KPCT("Tone", 60)}},
    [MK_HEDRA] = {"Hedr4", 45, "Three pitch-shifted voices in a rhythmic delay, in key (a scale) or free",
        {KST("Pitch 1", 7), KST("Pitch 2", 12), KST("Pitch 3", -12), KMS("Time", 20, 2000, 450), KCHO("Rhythm", RHYTHMS, 0),
         KPCT("Feedback", 30), KCHO("Key", KEYS, 0), KCHO("Scale", HD_SCALE, 0)}},
    [MK_AFTER] = {"Aftrneath", 50, "A swarm of short delays smeared into a strange reverb",
        {KMS("Length", 20, 400, 150), KPCT("Diffuse", 60), KPCT("Drag", 30), KPCT("Reflect", 50), KPCT("Mod", 30),
         KPCT("Tone", 50), KPCT("Width", 80), KINT("Density", 3, 8, 8, "")}},
    [MK_RAINBOW] = {"Rainbow Mchn", 50, "A pitch shifter that warbles, smears, tracks badly",
        {KST("Pitch", 12), KCHO("Second", SECOND, 0), KPCT("Tracking", 50), KPCT("Magic", 0), KPCT("Mod Depth", 20),
         KHZ("Mod Rate", 0.1f, 10, 3), KPCT("Tone", 60), KNONE}},
    [MK_DATA] = {"Data Korrupt", 60, "Square-wave synth on your notes, with subharmonics",
        {KST("Interval", 0), KCHO("Sub", SUBS, 0), KPCT("Square", 70), KPCT("Sub Level", 50), KPCT("Fuzz", 30),
         KMS("Glide", 1, 300, 20), KPCT("Vibrato", 0), KPCT("Tone", 50)}},
};

/* presets: a machine and its knobs (in their units), Mix */
typedef struct { const char *name; int machine; float mix; float k[NK]; } Preset;
static const Preset PRESETS[] = {
    /* Mikrocosm: 15 of its 44 effects, set as they come (Effect = kind x 4 + variation) */
    {"Mosaic A", MK_MICRO, 50, {0, 50, 50, 0.6f, 25, 12000, 35, 10}},
    {"Mosaic D", MK_MICRO, 55, {3, 75, 50, 0.8f, 30, 10000, 45, 10}},
    {"Seq B", MK_MICRO, 50, {5, 70, 60, 0.5f, 20, 9000, 30, 15}},
    {"Seq D", MK_MICRO, 50, {7, 85, 70, 0.4f, 20, 8000, 25, 20}},
    {"Glide A", MK_MICRO, 50, {8, 40, 50, 1.0f, 30, 11000, 45, 10}},
    {"Glide D", MK_MICRO, 50, {11, 60, 70, 1.2f, 25, 9000, 50, 10}},
    {"Haze A", MK_MICRO, 55, {12, 55, 40, 0.8f, 30, 9000, 70, 10}},
    {"Tunnel A", MK_MICRO, 55, {16, 50, 50, 0.6f, 35, 7000, 55, 15}},
    {"Tunnel B", MK_MICRO, 55, {17, 60, 50, 0.8f, 40, 6000, 60, 25}},
    {"Strum B", MK_MICRO, 50, {21, 60, 40, 0.5f, 30, 10000, 45, 10}},
    {"Blocks A", MK_MICRO, 55, {24, 60, 70, 0.5f, 15, 14000, 20, 10}},
    {"Interrupt B", MK_MICRO, 100, {29, 50, 75, 0.5f, 10, 16000, 15, 10}},
    {"Arp A", MK_MICRO, 50, {32, 50, 60, 0.4f, 25, 10000, 40, 15}},
    {"Pattern A", MK_MICRO, 45, {36, 40, 50, 0.45f, 35, 9000, 35, 10}},
    {"Warp B", MK_MICRO, 50, {41, 60, 50, 0.6f, 35, 3000, 35, 60}},
    /* Moodd MK2: Wet, Time, Modify, Loop, Length, Loop Mod (Tape: -2x -1x -0.5x 0.5x 1x 2x across it), Clock, Balance */
    {"Lush Loop", MK_MOOD, 50, {0, 60, 40, 0, 2, 50, 100, 50}},
    {"Tape Reverse", MK_MOOD, 55, {1, 45, 40, 1, 1.5f, 25, 100, 55}},
    {"Half Speed Haze", MK_MOOD, 55, {0, 70, 50, 1, 2, 55, 100, 55}},
    {"Octave Loops", MK_MOOD, 50, {1, 35, 30, 1, 1, 90, 100, 45}},
    {"Stretch Cloud", MK_MOOD, 55, {0, 80, 60, 2, 3, 70, 100, 55}},
    {"Frozen Moment", MK_MOOD, 60, {0, 90, 70, 2, 4, 90, 100, 60}},
    {"Duck Pad", MK_MOOD, 55, {0, 75, 50, 0, 2.5f, 80, 100, 60}},
    {"Slip Up", MK_MOOD, 50, {2, 40, 90, 1, 1.5f, 75, 100, 35}},
    {"Slip Down", MK_MOOD, 50, {2, 60, 55, 1, 2, 75, 100, 35}},
    {"Backwards Slip", MK_MOOD, 50, {2, 50, 20, 1, 1.5f, 75, 100, 40}},
    {"Lo-Fi Memory", MK_MOOD, 55, {1, 50, 45, 1, 2, 75, 35, 55}},
    {"Crushed Tape", MK_MOOD, 50, {1, 30, 30, 1, 1, 40, 15, 60}},
    {"Long Echoes", MK_MOOD, 45, {1, 75, 65, 0, 2, 30, 100, 25}},
    {"Ghost Loop", MK_MOOD, 55, {0, 85, 70, 1, 4, 25, 70, 65}},
    {"Slow Bloom", MK_MOOD, 60, {0, 95, 80, 2, 6, 85, 100, 50}},
    /* Partikle: Delay, Grain, Density, Pitch, Mode, Spread, Feedback, Tone */
    {"Scatter", MK_PARTICLE, 45, {400, 80, 12, 0, 0, 40, 30, 60}},
    {"Even Rain", MK_PARTICLE, 45, {300, 60, 20, 0, 1, 20, 35, 65}},
    {"Wobble Time", MK_PARTICLE, 45, {500, 100, 15, 0, 2, 50, 30, 60}},
    {"Pitch Sparks", MK_PARTICLE, 45, {600, 60, 18, 12, 3, 40, 40, 75}},
    {"Reverse Dust", MK_PARTICLE, 50, {700, 150, 10, 0, 4, 30, 35, 55}},
    {"Detune Cloud", MK_PARTICLE, 45, {350, 120, 25, 0, 5, 60, 30, 60}},
    {"Stairway", MK_PARTICLE, 45, {450, 80, 20, 7, 6, 30, 40, 65}},
    {"Siren Grains", MK_PARTICLE, 45, {500, 90, 16, 12, 7, 40, 35, 60}},
    {"Octave Down Dust", MK_PARTICLE, 50, {900, 150, 8, -12, 3, 50, 40, 40}},
    {"Fifth Shimmer", MK_PARTICLE, 45, {400, 70, 22, 7, 7, 30, 50, 75}},
    {"Backwards Octave", MK_PARTICLE, 50, {800, 120, 12, 12, 4, 30, 40, 60}},
    {"Tiny Grains", MK_PARTICLE, 45, {150, 15, 40, 0, 0, 70, 25, 70}},
    {"Huge Grains", MK_PARTICLE, 50, {1500, 300, 4, 0, 1, 20, 45, 50}},
    {"Broken Radio", MK_PARTICLE, 50, {250, 30, 30, -5, 3, 80, 30, 45}},
    {"Slow Swarm", MK_PARTICLE, 50, {1200, 250, 6, 0, 5, 50, 55, 50}},
    /* Tensr: Speed, Stretch, Stutter, Chance, Length, Direction, Pitch, Tone */
    {"Half Speed", MK_TENSOR, 60, {0.5f, 1, 4, 0, 1.5f, 0, 0, 55}},
    {"Reverse Tape", MK_TENSOR, 60, {1, 1, 4, 0, 2, 1, 0, 55}},
    {"Stretched", MK_TENSOR, 60, {1, 0.25f, 4, 0, 3, 0, 0, 60}},
    {"Stutters", MK_TENSOR, 60, {1, 1, 8, 60, 1, 0, 0, 65}},
    {"Back and Forth", MK_TENSOR, 60, {1, 1, 4, 0, 1.5f, 2, 0, 60}},
    {"Double Time", MK_TENSOR, 55, {2, 1, 4, 0, 1, 0, 0, 65}},
    {"Slow Reverse", MK_TENSOR, 60, {0.5f, 1, 4, 0, 2, 1, 0, 50}},
    {"Frozen Smear", MK_TENSOR, 60, {1, 0.25f, 2, 20, 4, 0, 0, 50}},
    {"Octave Freeze", MK_TENSOR, 55, {1, 0.25f, 4, 0, 3, 0, 12, 60}},
    {"Glitch Tape", MK_TENSOR, 60, {1, 1, 16, 80, 0.5f, 2, 0, 70}},
    {"Ratchet", MK_TENSOR, 60, {1, 1, 12, 100, 0.4f, 0, 0, 70}},
    {"Under Water", MK_TENSOR, 60, {0.5f, 0.5f, 2, 10, 2, 0, -12, 35}},
    {"Pitch Rise", MK_TENSOR, 55, {1, 0.5f, 4, 0, 2, 0, 7, 65}},
    {"Tape Chaos", MK_TENSOR, 60, {-1.5f, 0.75f, 6, 50, 1.5f, 2, 0, 60}},
    {"Lost Reel", MK_TENSOR, 60, {-0.5f, 0.4f, 3, 30, 4, 1, -5, 45}},
    /* NiteSky: Decay, Size, Shimmer, Pitch (+12 +7 +5 +19 -12), Filter, Reso, Seq Depth, Seq Rate */
    {"Night Shimmer", MK_NIGHT, 45, {8, 75, 40, 0, 1500, 40, 50, 1}},
    {"Star Steps", MK_NIGHT, 50, {6, 60, 25, 1, 900, 70, 90, 3}},
    {"Deep Night", MK_NIGHT, 50, {15, 90, 30, 4, 600, 30, 30, 0.3f}},
    {"Aurora", MK_NIGHT, 50, {20, 95, 55, 0, 3000, 20, 20, 0.2f}},
    {"Fifths Field", MK_NIGHT, 45, {10, 80, 45, 1, 2000, 35, 40, 0.5f}},
    {"Fourth Glow", MK_NIGHT, 45, {9, 70, 35, 2, 1800, 30, 40, 0.7f}},
    {"Twelfth Sky", MK_NIGHT, 45, {12, 85, 40, 3, 2500, 25, 30, 0.4f}},
    {"Seq Pulse", MK_NIGHT, 50, {4, 50, 10, 0, 800, 80, 100, 6}},
    {"Dark Matter", MK_NIGHT, 50, {25, 100, 20, 4, 400, 50, 20, 0.15f}},
    {"Morse Stars", MK_NIGHT, 50, {5, 55, 20, 0, 1200, 85, 100, 8}},
    {"Frozen Lake", MK_NIGHT, 50, {30, 100, 50, 0, 2500, 10, 0, 1}},
    {"Resonant Dusk", MK_NIGHT, 45, {7, 65, 25, 1, 700, 90, 60, 2}},
    {"Glass Choir", MK_NIGHT, 45, {14, 80, 70, 0, 4000, 30, 15, 0.3f}},
    {"Low Orbit", MK_NIGHT, 50, {11, 85, 30, 4, 900, 40, 50, 0.5f}},
    {"Meteor Rain", MK_NIGHT, 45, {6, 60, 30, 3, 3000, 60, 80, 4}},
    /* Cloudbrst: Decay, Size, Ensemble, Swell, Predelay, Tone, Low, Mod */
    {"Cloud Pad", MK_CLOUD, 45, {12, 80, 50, 30, 30, 55, 0, 40}},
    {"Infinite Swell", MK_CLOUD, 55, {40, 95, 70, 80, 80, 50, 2, 50}},
    {"Bright Air", MK_CLOUD, 40, {6, 60, 40, 10, 15, 80, -3, 30}},
    {"Small Room Glow", MK_CLOUD, 35, {1.5f, 30, 20, 0, 5, 60, 0, 20}},
    {"Plate Wash", MK_CLOUD, 40, {4, 55, 30, 10, 20, 70, -2, 30}},
    {"Cathedral", MK_CLOUD, 45, {20, 100, 30, 20, 60, 50, 3, 30}},
    {"Ensemble Sky", MK_CLOUD, 45, {10, 75, 100, 20, 40, 60, 0, 70}},
    {"Volume Swell", MK_CLOUD, 50, {8, 70, 40, 100, 30, 55, 0, 40}},
    {"Dark Halls", MK_CLOUD, 45, {18, 90, 40, 30, 50, 25, 4, 35}},
    {"Shine", MK_CLOUD, 40, {9, 70, 60, 20, 25, 90, -6, 40}},
    {"Long Predelay", MK_CLOUD, 40, {7, 65, 30, 10, 400, 60, 0, 30}},
    {"Warm Blanket", MK_CLOUD, 50, {15, 85, 60, 50, 40, 35, 6, 50}},
    {"Endless", MK_CLOUD, 55, {60, 100, 50, 60, 100, 50, 0, 50}},
    {"Choral Bloom", MK_CLOUD, 50, {25, 90, 90, 70, 60, 65, 0, 80}},
    {"Ambient Wash", MK_CLOUD, 50, {30, 95, 70, 40, 80, 45, 2, 60}},
    /* Polym00n: 15 of the factory presets (their names and tempos), set from what the names say */
    {"MultiplyOne", MK_POLY, 45, {760, 45, 1, 20, 0.5f, 0, 0, 60}},
    {"MultiMem", MK_POLY, 45, {1200, 55, 6, 30, 0.4f, 10, 0, 55}},
    {"LateShift", MK_POLY, 45, {1200, 50, 3, 60, 0.2f, 0, 20, 50}},
    {"DarkMod", MK_POLY, 45, {720, 50, 3, 70, 1.2f, 20, 0, 25}},
    {"Togglephase", MK_POLY, 45, {860, 45, 2, 30, 0.8f, 90, 0, 60}},
    {"DarkDimension", MK_POLY, 45, {770, 55, 6, 40, 0.6f, 20, 10, 30}},
    {"PhaserPan", MK_POLY, 45, {770, 40, 4, 50, 1.5f, 80, 0, 60}},
    {"ShiftFlange", MK_POLY, 45, {1200, 50, 3, 50, 0.3f, 0, 80, 60}},
    {"SpectralDecay", MK_POLY, 45, {1200, 70, 5, 25, 0.3f, 40, 40, 45}},
    {"Elevate", MK_POLY, 45, {1200, 65, 6, 20, 0.2f, 30, 20, 80}},
    {"SlowPhase", MK_POLY, 45, {340, 35, 2, 30, 0.1f, 90, 0, 60}},
    {"DynamicEcho", MK_POLY, 45, {760, 45, 1, 15, 0.7f, 0, 0, 55}},
    {"NativeFlange", MK_POLY, 45, {420, 40, 2, 40, 0.5f, 0, 90, 65}},
    {"GravityHarp", MK_POLY, 45, {1200, 75, 6, 35, 0.15f, 20, 30, 70}},
    {"DimensionChorus", MK_POLY, 45, {760, 30, 5, 70, 1.0f, 0, 20, 60}},
    /* Hedr4: 15 of the factory presets (names and tempos); Pitch 1-3, Time, Rhythm, Feedback, Key, Scale (0 = Off) */
    {"Series Sequence", MK_HEDRA, 45, {7, 5, 7, 520, 4, 30, 0, 1}},
    {"Pitch Echo", MK_HEDRA, 45, {12, 0, 0, 520, 0, 45, 0, 0}},
    {"Micro Fall", MK_HEDRA, 45, {-1, -2, -3, 130, 0, 55, 0, 0}},
    {"Micro Chorus", MK_HEDRA, 45, {0, 0, 0, 130, 0, 20, 0, 0}},
    {"Hard Tune Triplet", MK_HEDRA, 45, {0, 7, 12, 180, 1, 20, 0, 1}},
    {"Stereo Octave", MK_HEDRA, 45, {12, -12, 0, 260, 0, 25, 0, 0}},
    {"Carle Place Shred", MK_HEDRA, 45, {4, 7, 12, 210, 1, 35, 0, 1}},
    {"Expression Steel", MK_HEDRA, 45, {2, 5, 7, 520, 2, 40, 0, 0}},
    {"Tick Tock", MK_HEDRA, 45, {12, -12, 7, 520, 3, 35, 0, 0}},
    {"Layers", MK_HEDRA, 45, {7, 12, 19, 540, 0, 50, 0, 0}},
    {"Expression Shift", MK_HEDRA, 50, {12, 12, 12, 20, 0, 0, 0, 0}},
    {"Pentatonic Correct", MK_HEDRA, 45, {3, 7, 12, 220, 0, 30, 9, 7}},
    {"Infinite Arcade", MK_HEDRA, 45, {12, 19, 24, 70, 0, 85, 0, 0}},
    {"Mirror Glide", MK_HEDRA, 45, {5, -7, 12, 260, 2, 40, 0, 0}},
    {"Bumble Trail", MK_HEDRA, 45, {-5, -12, 7, 190, 1, 55, 0, 0}},
    /* Aftrneath: Length, Diffuse, Drag, Reflect, Mod, Tone, Width, Density */
    {"Underneath", MK_AFTER, 50, {150, 60, 30, 50, 30, 50, 80, 8}},
    {"Long Drag", MK_AFTER, 55, {350, 70, 90, 60, 40, 45, 100, 8}},
    {"Small Swarm", MK_AFTER, 45, {40, 40, 20, 40, 20, 60, 60, 5}},
    {"Cavern", MK_AFTER, 55, {400, 80, 50, 75, 30, 40, 100, 8}},
    {"Spring Ghost", MK_AFTER, 45, {60, 30, 10, 60, 50, 70, 70, 4}},
    {"Metal Box", MK_AFTER, 45, {25, 20, 0, 70, 10, 80, 50, 3}},
    {"Drift Wood", MK_AFTER, 50, {200, 60, 70, 55, 70, 45, 90, 6}},
    {"Infinite Swarm", MK_AFTER, 55, {300, 75, 40, 90, 40, 50, 100, 8}},
    {"Bright Swarm", MK_AFTER, 50, {120, 50, 30, 50, 30, 85, 80, 7}},
    {"Dark Well", MK_AFTER, 50, {250, 70, 60, 70, 20, 25, 80, 8}},
    {"Seasick", MK_AFTER, 50, {180, 50, 30, 50, 100, 50, 90, 6}},
    {"Tight Slap", MK_AFTER, 40, {30, 10, 0, 20, 10, 70, 40, 3}},
    {"Smear Field", MK_AFTER, 55, {280, 90, 80, 65, 50, 50, 100, 8}},
    {"Ghost Rhythm", MK_AFTER, 45, {220, 20, 0, 60, 20, 60, 80, 4}},
    {"Under Ice", MK_AFTER, 50, {160, 85, 50, 80, 15, 30, 70, 8}},
    /* Rainbow Mchn: Pitch, Second (Off, Oct Down, Unison, Fifth, Oct Up), Tracking, Magic, Mod Depth, Mod Rate, Tone */
    {"Rainbow Up", MK_RAINBOW, 50, {12, 0, 50, 0, 20, 3, 60, 0}},
    {"Magic Rise", MK_RAINBOW, 50, {7, 0, 30, 70, 15, 2, 55, 0}},
    {"Broken Octaves", MK_RAINBOW, 55, {12, 1, 100, 20, 40, 6, 50, 0}},
    {"Down Below", MK_RAINBOW, 50, {-12, 0, 30, 0, 10, 2, 45, 0}},
    {"Fifth Wobble", MK_RAINBOW, 50, {7, 0, 60, 10, 50, 4, 60, 0}},
    {"Chorus Ghost", MK_RAINBOW, 50, {0, 2, 40, 0, 40, 1, 60, 0}},
    {"Infinite Climb", MK_RAINBOW, 50, {12, 0, 20, 85, 10, 1, 65, 0}},
    {"Falling Stairs", MK_RAINBOW, 50, {-5, 0, 30, 80, 10, 1, 50, 0}},
    {"Organ Stack", MK_RAINBOW, 55, {12, 3, 20, 0, 10, 5, 60, 0}},
    {"Sub Octave", MK_RAINBOW, 55, {-12, 1, 10, 0, 5, 2, 40, 0}},
    {"Seasick Shift", MK_RAINBOW, 50, {5, 0, 80, 30, 80, 0.5f, 55, 0}},
    {"Vibrato Glass", MK_RAINBOW, 50, {12, 4, 40, 20, 60, 6, 75, 0}},
    {"Detuned Twin", MK_RAINBOW, 50, {0, 4, 70, 10, 30, 3, 60, 0}},
    {"Magic Fourths", MK_RAINBOW, 50, {5, 0, 30, 70, 20, 2, 60, 0}},
    {"Sparkle", MK_RAINBOW, 45, {24, 0, 60, 30, 30, 4, 80, 0}},
    /* Data Korrupt: Interval, Sub (/2 /3 /4 /5), Square, Sub Level, Fuzz, Glide, Vibrato, Tone */
    {"Corrupt Sub", MK_DATA, 60, {0, 0, 70, 50, 30, 20, 0, 50}},
    {"Square Lead", MK_DATA, 70, {12, 0, 90, 20, 10, 40, 30, 70}},
    {"Deep Divide", MK_DATA, 70, {-12, 2, 50, 80, 20, 10, 0, 35}},
    {"Fifth Synth", MK_DATA, 65, {7, 0, 80, 40, 10, 30, 10, 60}},
    {"Third Sub", MK_DATA, 65, {0, 1, 60, 70, 20, 20, 0, 45}},
    {"Glide Bass", MK_DATA, 70, {-12, 0, 60, 70, 10, 200, 0, 35}},
    {"Fuzz Monster", MK_DATA, 60, {0, 0, 50, 50, 90, 20, 0, 45}},
    {"Vibrato Lead", MK_DATA, 70, {12, 0, 90, 10, 0, 60, 70, 65}},
    {"Fifth Below", MK_DATA, 65, {-7, 3, 60, 60, 20, 20, 0, 45}},
    {"Octave Square", MK_DATA, 65, {12, 2, 80, 40, 30, 10, 0, 70}},
    {"8-Bit Choir", MK_DATA, 60, {0, 0, 70, 30, 0, 5, 40, 80}},
    {"Broken Bass", MK_DATA, 70, {-24, 0, 40, 90, 40, 30, 0, 30}},
    {"Bell Sub", MK_DATA, 60, {19, 1, 50, 50, 0, 20, 20, 75}},
    {"Dirty Unison", MK_DATA, 60, {0, 0, 60, 0, 70, 10, 0, 55}},
    {"Slow Sweep", MK_DATA, 65, {0, 3, 70, 60, 20, 300, 30, 40}},
};
#define NPRESETS ((int)(sizeof PRESETS / sizeof *PRESETS))
/* a machine's presets: the j-th of machine m (its index in PRESETS, or -1), how many it has, and a preset's place */
static int preset_of(int m, int j) {
    for (int i = 0, n = 0; i < NPRESETS; i++) if (PRESETS[i].machine == m && n++ == j) return i;
    return -1;
}
static int presets_in(int m) { int n = 0; for (int i = 0; i < NPRESETS; i++) n += PRESETS[i].machine == m; return n; }
static int place_of(int idx) { int n = 0; for (int i = 0; i < idx; i++) n += PRESETS[i].machine == PRESETS[idx].machine; return n; }

enum { kMachine, kMachineOpen, kMix, kK, kHold = kK + NK, kPreset, kPresetPrev, kPresetNext, kOutput, kNumParams };

static float knob_value(const Knob *k, float n) {
    n = clampf(n, 0, 1);
    switch (k->map) {
    case M_EXP: return expmap(k->lo, k->hi, n);
    case M_INT: case M_CHOICE: return roundf(lerp(k->lo, k->hi, n));
    default: return lerp(k->lo, k->hi, n);
    }
}
static float knob_norm(const Knob *k, float v) {
    if (k->map == M_EXP) return clampf(logf(v / k->lo) / logf(k->hi / k->lo), 0, 1);
    return k->hi > k->lo ? clampf((v - k->lo) / (k->hi - k->lo), 0, 1) : 0;
}

/* ==== building blocks ===================================================================================== */
typedef struct { float ic1, ic2; } Svf;   /* TPT state-variable filter */
static float svf_lp(Svf *f, float x, float g, float k) {
    const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float v3 = x - f->ic2, v1 = a1 * f->ic1 + a2 * v3, v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2 * v1 - f->ic1; f->ic2 = 2 * v2 - f->ic2;
    return v2;
}
static float svf_bp(Svf *f, float x, float g, float k) {   /* the same filter's band-pass output */
    const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float v3 = x - f->ic2, v1 = a1 * f->ic1 + a2 * v3, v2 = f->ic2 + a2 * f->ic1 + a3 * v3;
    f->ic1 = 2 * v1 - f->ic1; f->ic2 = 2 * v2 - f->ic2;
    return v1;
}
/* a plain two-grain pitch shifter (50 ms grains); ratio > 1 up */
typedef struct { Line l; float ph; } Shift;
static float hann01(float p) { const float s = sinf(PI * p); return s * s; }
static float shift(Shift *s, float x, float ratio, float win) {
    line_push(&s->l, x);
    s->ph += (1 - ratio) / win;
    s->ph -= floorf(s->ph);
    float p2 = s->ph + 0.5f;
    p2 -= floorf(p2);
    return line_read(&s->l, s->ph * win + 2) * hann01(s->ph) + line_read(&s->l, p2 * win + 2) * hann01(p2);
}
/* grains reading the recording: d = samples ago; it moves by (recording ? 1 : 0) - rate each sample. The rate can
 * glide from r0 to r1 over the grain (sm: 0 straight, 1 eased); the envelope rises over atk of it (0.5: symmetric,
 * and overlapping halves then add to exactly one); bus: which output it sums into (Moodd: wet / loop) */
#define NGR 48
typedef struct { float d, r0, r1, len, age, pan, gain, atk, sm; unsigned char on, bus; } Grain;
static float smooth01(float u) { u = clampf(u, 0, 1); return u * u * (3 - 2 * u); }
static float genv(float p, float atk) { return smooth01(p < atk ? p / atk : (1 - p) / (1 - atk)); }
typedef struct { float ph; } Lfo;
static float sine01(float ph) { return sinf(2 * PI * ph); }

typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    int ready, machine, preset;
    volatile int update_display;
    volatile uint32_t automate;
    Line rec[2], dl[2], dl2[2], ap[2][6];
    int apd[2][6];
    Verb verb;
    Shift sh[2][3];
    Grain gr[NGR];
    Tracker trk;
    /* Mikrocosm */
    float mc_g, mc_next, mc_anchor, mc_pend, mc_ianch, mc_head, mc_fmod, mc_ftarget, mc_gl, mc_squeeze, mc_y[2], mc_ph[3];
    float crush_ph, crush_v[2];
    int mc_step, mc_glitch, mc_burst, mc_bj;
    float on_fast, on_slow, on_age[8];   /* onsets: the last eight, samples ago */
    int on_head, on_count, on_refr, onset;
    /* Moodd MK2 */
    float md_anchor, md_pos, md_next, md_snext, md_lp[2];
    /* Hedr4: the pitch heard in the line, every 512 samples, back ~11 s (MIDI note; < 0: no pitch) */
    float hd_m[1024], hd_ratio[3];
    int hd_w, hd_cnt;
    OnePole op[2][8];
    Svf svf[2];
    float st[2][16], g[24], lfo[6], timer, mix_s, out_s, env, seq[8];
    float v[NK];   /* the knobs in their units (worked out every 16 samples) */
    int tick, hold;
    float dly_s[8];
} Plugin;

static void reset(Plugin *s) {
    for (int c = 0; c < 2; c++) {
        line_clear(&s->rec[c]); line_clear(&s->dl[c]); line_clear(&s->dl2[c]);
        for (int k = 0; k < 6; k++) line_clear(&s->ap[c][k]);
        for (int k = 0; k < 3; k++) { line_clear(&s->sh[c][k].l); s->sh[c][k].ph = 0; }
        memset(s->op[c], 0, sizeof s->op[c]);
        memset(s->st[c], 0, sizeof s->st[c]);
        s->svf[c].ic1 = s->svf[c].ic2 = 0;
    }
    verb_clear(&s->verb);
    memset(s->gr, 0, sizeof s->gr);
    memset(&s->trk, 0, sizeof s->trk);
    memset(s->lfo, 0, sizeof s->lfo);
    s->timer = 0; s->env = 0;
    for (int k = 0; k < 8; k++) s->dly_s[k] = 0;
    s->mc_next = s->mc_anchor = s->mc_pend = s->mc_ianch = s->mc_head = s->mc_fmod = s->mc_ftarget = s->mc_gl = 0;
    s->mc_squeeze = 1; s->mc_y[0] = s->mc_y[1] = 0; s->crush_ph = 0; s->crush_v[0] = s->crush_v[1] = 0;
    s->mc_step = s->mc_glitch = s->mc_burst = s->mc_bj = 0;
    s->on_fast = s->on_slow = 0; s->on_head = s->on_count = s->on_refr = s->onset = 0;
    for (int k = 0; k < 8; k++) s->on_age[k] = 0;
    s->md_anchor = s->md_pos = s->md_next = s->md_snext = 0; s->md_lp[0] = s->md_lp[1] = 0;
    for (int k = 0; k < 1024; k++) s->hd_m[k] = -1;
    s->hd_ratio[0] = s->hd_ratio[1] = s->hd_ratio[2] = 1; s->hd_w = s->hd_cnt = 0;
}
static void free_all(Plugin *s) {
    for (int c = 0; c < 2; c++) {
        line_free(&s->rec[c]); line_free(&s->dl[c]); line_free(&s->dl2[c]);
        for (int k = 0; k < 6; k++) line_free(&s->ap[c][k]);
        for (int k = 0; k < 3; k++) line_free(&s->sh[c][k].l);
    }
    verb_free(&s->verb);
}
static int setup(Plugin *s, float fs) {
    s->ready = 0;
    free_all(s);
    s->fs = fs;
    int ok = verb_init(&s->verb, fs * 1.0f);
    for (int c = 0; c < 2; c++) {
        ok &= line_init(&s->rec[c], (int)(fs * 10));
        ok &= line_init(&s->dl[c], (int)(fs * 3));
        ok &= line_init(&s->dl2[c], (int)(fs * 0.6f));
        for (int k = 0; k < 6; k++) ok &= line_init(&s->ap[c][k], (int)(fs * 0.12f));
        for (int k = 0; k < 3; k++) ok &= line_init(&s->sh[c][k].l, (int)(fs * 0.12f));
    }
    reset(s);
    s->ready = ok;
    return ok;
}

/* a grain that may glide; d is kept where the whole grain stays inside the recording, held or not */
static void spawn_g(Plugin *s, float d, float r0, float r1, float len, float pan, float gain, float atk, float sm, int bus) {
    if (len < 16) len = 16;
    const float maxd = (float)s->rec[0].mask - 8, hi = fmaxf(fmaxf(r0, r1), 0), lo = fminf(fminf(r0, r1), 0);
    d = clampf(d, len * hi + 4, maxd - len * (1 - lo));
    if (d < 4) return;
    for (int i = 0; i < NGR; i++)
        if (!s->gr[i].on) { s->gr[i] = (Grain){d, r0, r1, len, 0, pan, gain, clampf(atk, 0.02f, 0.98f), sm, 1, (unsigned char)bus}; return; }
}
static void spawn(Plugin *s, float d, float rate, float len, float pan, float gain) { spawn_g(s, d, rate, rate, len, pan, gain, 0.5f, 0, 0); }
static void run_grains_bus(Plugin *s, int recording, float o[2][2]) {
    o[0][0] = o[0][1] = o[1][0] = o[1][1] = 0;
    const float maxd = (float)s->rec[0].mask - 4;
    for (int i = 0; i < NGR; i++) {
        Grain *g = &s->gr[i];
        if (!g->on) continue;
        const float p = g->age / g->len, w = genv(p, g->atk) * g->gain;
        const float d = clampf(g->d, 1, maxd);
        o[g->bus][0] += line_read(&s->rec[0], d) * w * (1 - g->pan);
        o[g->bus][1] += line_read(&s->rec[1], d) * w * (1 + g->pan);
        float rate = g->r0;
        if (g->r1 != g->r0) rate += (g->r1 - g->r0) * (p + g->sm * (smooth01(p) - p));
        g->d += (recording ? 1.0f : 0.0f) - rate;
        if (++g->age >= g->len || g->d < 1 || g->d > maxd) g->on = 0;
    }
}
static void run_grains(Plugin *s, int recording, float *ol, float *or_) {
    float o[2][2];
    run_grains_bus(s, recording, o);
    *ol = o[0][0]; *or_ = o[0][1];
}
static float allpass(Line *l, int d, float x, float g) {
    const float v = line_tap(l, d - 1), u = x + g * v;
    line_push(l, u);
    return v - g * u;
}

/* per-machine values that don't change every sample */
static void control(Plugin *s) {
    const Machine *m = &MACHINES[s->machine];
    for (int i = 0; i < NK; i++) s->v[i] = m->k[i].name ? knob_value(&m->k[i], s->norm[kK + i]) : 0;
    const float fs = s->fs, *v = s->v;
    float *g = s->g;
    switch (s->machine) {
    case MK_MICRO: {
        verb_set(&s->verb, 1.5f, s->hold ? 30 : lerp(1.5f, 9, v[6] * 0.01f), 8000, fs);
        /* the filter's movement for the effects that sweep it (LFOs advance 16 samples at a time) */
        const int fx = (int)v[0], fam = fx >> 2, var = fx & 3;
        const float a = v[1] * 0.01f, T = v[3], dt = 16 / fs;
        for (int k = 0; k < 3; k++) { s->mc_ph[k] += dt * (k == 0 ? 1 / (8 * T) : k == 1 ? 0.12f : 0.15f); s->mc_ph[k] -= floorf(s->mc_ph[k]); }
        if (fam == F_SEQ && var == 2) s->mc_ftarget = -1.5f * (0.5f + 0.5f * sine01(s->mc_ph[0]));
        else if (fam == F_TUNNEL && (var == 1 || var == 2)) s->mc_ftarget = -2 * a * (0.5f + 0.5f * sine01(s->mc_ph[1]));
        else if (fam == F_WARP && var == 1) s->mc_ftarget = -2 * a * (0.5f + 0.5f * sine01(s->mc_ph[1]));
        else if (fam == F_WARP && var == 0) s->mc_ftarget = -3 * a + clampf(s->on_fast * 12, 0, 1) * 3 * a;   /* opens as you play */
        else if (fam == F_INTERRUPT && var == 2) s->mc_ftarget = s->mc_glitch > 0 ? s->mc_ftarget - dt * 3 : 0;   /* sweeps down in a glitch */
        else if (fam == F_BLOCKS && var == 2) s->mc_ftarget = -1.2f;
        else if (!((fam == F_SEQ && var == 0) || (fam == F_ARP && var == 2))) s->mc_ftarget = 0;   /* (those set it per step) */
        if (fam == F_TUNNEL && var == 0) s->mc_squeeze = 1 + 0.8f * a * sine01(s->mc_ph[2]);
        else if (fam == F_TUNNEL && var == 3) s->mc_squeeze += (1 - s->mc_squeeze) * dt * 1.5f;
        else s->mc_squeeze = 1;
        break;
    }
    case MK_MOOD: {
        const int wet = (int)v[0];
        const float t = v[1] * 0.01f, m = v[2] * 0.01f;
        if (wet == 0) verb_set(&s->verb, lerp(0.7f, 2.0f, m), s->hold ? 60 : expmap(0.4f, 20, t), expmap(2500, 9000, 1 - 0.5f * m), fs);
        g[0] = lp_coef(6000, fs);
        break;
    }
    case MK_PARTICLE: case MK_TENSOR: g[0] = lp_coef(expmap(1500, 16000, v[7] * 0.01f), fs); break;
    case MK_NIGHT: {
        const float size = lerp(0.8f, 2.0f, v[1] * 0.01f), rt = s->hold ? 1000 : v[0];
        verb_set(&s->verb, size, rt, 7000, fs);
        /* the shimmer goes round the reverb again: scaled by what the reverb loses each pass, it can't run away */
        g[2] = 1 - powf(10, -3 * 0.0474f * size / rt);
        break;
    }
    case MK_CLOUD: verb_set(&s->verb, lerp(0.9f, 2.0f, v[1] * 0.01f), s->hold ? 1000 : v[0], expmap(2000, 14000, v[5] * 0.01f), fs);
        g[0] = lp_coef(200, fs); g[1] = db(v[6]) - 1; break;
    case MK_POLY: case MK_DATA: g[0] = lp_coef(expmap(1000, 16000, v[7] * 0.01f), fs); g[1] = lp_coef(25, fs); break;   /* Tone: knob 8 */
    case MK_HEDRA: g[0] = lp_coef(9000, fs); break;
    case MK_AFTER: g[0] = lp_coef(expmap(1000, 16000, v[5] * 0.01f), fs); break;                               /* knob 6 */
    case MK_RAINBOW: g[0] = lp_coef(expmap(1000, 16000, v[6] * 0.01f), fs); break;                             /* knob 7 */
    default: break;
    }
    if (s->machine == MK_AFTER) {   /* the swarm's diffusers */
        for (int c = 0; c < 2; c++) for (int k = 0; k < 6; k++) {
            static const float MS[6] = {3.1f, 4.7f, 7.3f, 11.3f, 17.9f, 23.1f};
            s->apd[c][k] = (int)(MS[k] * (c ? 1.11f : 1) * fs / 1000 * lerp(0.6f, 2.5f, v[0] / 400));
            if (s->apd[c][k] < 2) s->apd[c][k] = 2;
            if (s->apd[c][k] > s->ap[c][k].mask - 2) s->apd[c][k] = s->ap[c][k].mask - 2;
        }
    }
}


/* ==== Mikrocosm: the 44 effects ============================================================================
 * Everything plays grains out of the recording (rec, ~10 s): loops, steps, glides, clouds, drones, strums of your
 * notes (an onset detector keeps the last eight), rearranged blocks, glitches, arpeggios, tap patterns. Then a
 * filter (Filter / Reso; some effects move it), bit-crush (the D's of Seq, Blocks, Interrupt, Arp), Repeats (fed
 * back into the recording) and Space (reverb). Time is the beat everything is measured in. */
static void onsets(Plugin *s, const float in[2], int rec) {
    const float x = 0.5f * (fabsf(in[0]) + fabsf(in[1]));
    s->on_fast += (x > s->on_fast ? 0.02f : 0.001f) * (x - s->on_fast);
    s->on_slow += (x > s->on_slow ? 0.0007f : 0.00007f) * (x - s->on_slow);
    if (rec) for (int k = 0; k < 8; k++) s->on_age[k] += 1;
    s->onset = 0;
    if (s->on_refr > 0) s->on_refr--;
    else if (s->on_fast > 1.8f * s->on_slow + 0.01f) {
        s->on_head = (s->on_head + 1) & 7; s->on_age[s->on_head] = 0; s->on_count++;
        s->on_refr = (int)(0.08f * s->fs); s->onset = 1;
    }
}
static void micro_events(Plugin *s, int rec) {
    const float fs = s->fs, *v = s->v;
    const int fx = (int)v[0], fam = fx >> 2, var = fx & 3;
    const float a = v[1] * 0.01f, sh = v[2] * 0.01f, T = v[3] * fs, span = fminf(2 * T, 9 * fs);
    const float atk = lerp(0.85f, 0.04f, sh);
    if (rec) { s->mc_anchor += 1; s->mc_ianch += 1; }
    if (fam == F_TUNNEL) {   /* the drone's segment: caught a moment after each new note, refreshed when old */
        const float seg = clampf(T * 0.5f, 0.04f * fs, fs);
        if (s->onset && s->mc_pend <= 0) { s->mc_pend = seg; if (var == 3) s->mc_squeeze = 0.25f; }
        if (s->mc_pend > 0 && rec && (s->mc_pend -= 1) <= 0) s->mc_anchor = 0;
        if (s->mc_anchor > 8 * fs) s->mc_anchor = 0;
    }
    if (fam == F_HAZE && var == 0) {   /* a stretch head, a quarter speed through the past */
        s->mc_head += (rec ? 1.0f : 0.0f) - 0.25f;
        if (s->mc_head > span || s->mc_head < 0.05f * fs) s->mc_head = 0.05f * fs + 1;
    }
    if ((s->mc_next -= 1) > 0) return;
    const float pan1 = (rnd() * 2 - 1) * 0.6f;
    s->mc_g = 1.2f;   /* how loud the grains can add up (in phase): Repeats is divided by it, so its loop stays under one */
    switch (fam) {
    case F_MOSAIC: {   /* loopers of the last beat, each at its speed */
        static const float SP[4][4] = {{1, 2, 1, 2}, {1, 0.5f, 1, 0.5f}, {2, 2, 2, 2}, {0.5f, 1, 2, 4}};
        const int n = 1 + (int)(a * 3.99f);
        for (int i = 0; i < n; i++) {
            const float r = SP[var][i];
            spawn_g(s, T * r + 4 + (float)i * 0.37f * T, r, r, T, i ? (i & 1 ? 0.6f : -0.6f) : 0, 0.75f / sqrtf((float)n), 0.5f, 0, 0);
        }
        s->mc_g = 0.75f * sqrtf((float)n);
        s->mc_next += T * 0.5f;
        break;
    }
    case F_SEQ: {   /* steps of a quarter beat from anywhere in the last two beats */
        const float step = fmaxf(T * 0.25f, 0.02f * fs), len = step * (var == 2 ? 3 : 1.5f);
        s->mc_step++;
        if (rnd() < 0.35f + 0.65f * a) {
            float r = 1;
            if (var == 0) s->mc_ftarget = -2 * rnd();
            if (var == 1) r = (s->mc_step & 1) ? 0.5f : 1;
            if (var == 3) r = rnd() < 0.5f ? 1 : 2;
            spawn_g(s, len * r + 4 + rnd() * span, r, r, len, pan1, var == 2 ? 0.55f : 0.8f, atk, 0, 0);
        }
        if (var == 1 && a > 0.9f && (s->mc_step & 3) == 0) spawn_g(s, 2 * T + 4, 1, 1, 2 * T, 0, 0.35f, 0.5f, 0, 0);   /* the sustainer */
        if (var == 3 && a > 0.9f && (s->mc_step & 1)) spawn_g(s, step * 2 + 4 + rnd() * span, 0.5f, 0.5f, step * 3, 0, 0.5f, 0.5f, 0, 0);   /* the sub-octave */
        s->mc_next += step;
        break;
    }
    case F_GLIDE: {   /* grains gliding between speeds; Shape eases the glide */
        static const float LO[3] = {0.5f, 0.5f, 1}, HI[3] = {1, 2, 2};
        const float len = T * lerp(2, 0.4f, a);
        s->mc_step++;
        if (var < 3) {
            const int up = s->mc_step & 1;
            const float r0 = var == 1 ? (up ? 2 : 0.5f) : up ? LO[var] : HI[var], r1 = var == 1 ? (up ? 0.5f : 2) : up ? HI[var] : LO[var];
            spawn_g(s, len * fmaxf(r0, r1) + 4 + rnd() * T * 0.5f, r0, r1, len, pan1, 0.8f, 0.5f, sh, 0);
        } else {
            spawn_g(s, len * 2 + 4 + rnd() * T * 0.5f, 0.5f, 2, len, -0.5f, 0.6f, 0.5f, sh, 0);
            spawn_g(s, len * 2 + 4 + rnd() * T * 0.5f, 2, 0.5f, len, 0.5f, 0.6f, 0.5f, sh, 0);
        }
        s->mc_next += len * 0.5f;
        break;
    }
    case F_HAZE: {   /* a cloud of short grains; Activity = how many */
        const float glen = clampf(T * 0.25f, 0.02f * fs, 0.4f * fs) * (var == 0 ? 2.5f : 1);
        const float dens = lerp(4, 60, a) * (var == 1 ? 1.6f : 1), overlap = dens * glen / fs;
        const float r = var == 2 ? (rnd() < 0.5f ? 1 : 2) : var == 3 ? (rnd() < 0.5f ? 1 : 0.5f) : 1;
        const float d = var == 0 ? s->mc_head + glen + rnd() * 0.05f * fs : glen * r + 4 + rnd() * span * (var == 1 ? 1 : 0.5f);
        spawn_g(s, d, r, r, glen, (rnd() * 2 - 1) * 0.8f, 0.9f / sqrtf(1 + overlap), atk, 0, 0);
        s->mc_next += fs / dens * (0.5f + rnd());
        break;
    }
    case F_TUNNEL: {   /* a drone: long, overlapping grains looping the caught segment */
        const float seg = clampf(T * 0.5f, 0.04f * fs, fs), segm = seg * s->mc_squeeze;
        s->mc_step++;
        const float r = var == 1 && (s->mc_step & 1) ? 0.5f : 1, len = segm * lerp(0.4f, 1, sh);
        spawn_g(s, s->mc_anchor + len * r + rnd() * fmaxf(seg - len * r, 0), r, r, len, pan1 * 0.5f, 0.5f, 0.5f, 0, 0);
        s->mc_next += len * 0.25f;
        break;
    }
    case F_STRUM: {   /* your notes again: the last one, copies of it, or a cascade back through the last few */
        if (s->on_count == 0) { s->mc_next += 0.05f * fs; break; }
        s->mc_step++;
        if (var == 0) {
            const float step = T * lerp(1, 0.2f, a), len = step * 1.2f;
            if (s->on_age[s->on_head] < 9 * fs) spawn_g(s, fmaxf(s->on_age[s->on_head], len + 4), 1, 1, len, pan1, 0.8f, atk, 0, 0);
            s->mc_next += step;
        } else if (var == 1) {
            const float step = T * lerp(0.5f, 0.08f, a), len = T * 1.5f, r = 1 + (rnd() - 0.5f) * 0.012f;
            if (s->on_age[s->on_head] < 9 * fs) spawn_g(s, fmaxf(s->on_age[s->on_head], len + 4), r, r, len, pan1, 0.9f / sqrtf(1 + len / step), atk, 0, 0);
            s->mc_next += step;
        } else {
            const float step = T * lerp(0.5f, 0.12f, a), r = var == 3 && (s->mc_step & 1) ? 2 : 1, len = step * 2;
            const int n = s->on_count < 8 ? s->on_count : 8, idx = (s->on_head - s->mc_step % n + 8) & 7;
            if (s->on_age[idx] < 9 * fs) spawn_g(s, fmaxf(s->on_age[idx], len * r + 4), r, r, len, (s->mc_step & 1) ? 0.6f : -0.6f, 0.6f, atk, 0, 0);
            s->mc_next += step;
        }
        break;
    }
    case F_BLOCKS: {   /* the last two beats cut in eight and played in another order */
        const float blk = fmaxf(T * 0.25f, 0.03f * fs);
        float len = blk * 1.3f, r = 1, step = blk;
        int j;
        if (s->mc_burst > 0) { s->mc_burst--; j = s->mc_bj; step = blk * 0.25f; len = step * 1.3f; }
        else {
            j = rnd() < a ? (int)(rnd() * 7.99f) : 0;
            if (var == 0 && rnd() < a * 0.3f) { s->mc_burst = 3; s->mc_bj = j; }
        }
        if (var == 1 && rnd() < a) { static const float R[4] = {2, 0.5f, 1.5f, 0.75f}; r = R[(int)(rnd() * 3.99f)]; }
        if (var == 3 && rnd() < 0.6f) r = rnd() < 0.5f ? 2 : 0.5f;
        spawn_g(s, (float)j * blk + len * fmaxf(r, 1) + 4, r, r, len, pan1 * 0.5f, 0.85f, var == 2 ? fmaxf(atk, 0.4f) : atk, 0, 0);
        s->mc_next += step;
        break;
    }
    case F_INTERRUPT: {   /* your playing, broken now and then by glitches cut from what you just played */
        const float beat = fmaxf(T * 0.25f, 0.03f * fs), subs = var == 3 ? 4 : 2, sub = beat / subs;
        if (s->mc_glitch <= 0) {
            s->mc_next += beat;
            if (rnd() < a * 0.6f) { s->mc_glitch = (int)((1 + (int)(rnd() * 3.99f)) * subs); s->mc_ianch = 0; }
        } else {
            s->mc_glitch--;
            float r = 1;
            if (var == 1) r = rnd() < 0.5f ? 2 : 0.5f;
            if (var == 3) { static const float R[4] = {4, 0.25f, 2, 1}; r = R[(int)(rnd() * 3.99f)]; }
            const float len = sub * 1.1f;
            spawn_g(s, s->mc_ianch + (float)(int)(rnd() * 3.99f) * sub + len * fmaxf(r, 1) + 4, r, r, len, pan1 * 0.4f, 1, atk, 0, 0);
            s->mc_next += sub;
        }
        break;
    }
    case F_ARP: {   /* the latest sound replayed up a pattern of pitches (B: of speeds); Activity = how many steps */
        static const float ST[8] = {0, 7, 12, 7, 0, 12, 19, 12};
        static const float SPD[8] = {1, -1, 0.5f, 2, 1.5f, -0.5f, 2, 0.75f};
        const int n = 2 + (int)(a * 6.99f), idx = s->mc_step % n;
        const float step = fmaxf(T * 0.25f, 0.02f * fs), len = step * 1.3f;
        s->mc_step++;
        const float r = var == 1 ? SPD[idx] : exp2f(ST[idx] / 12);
        if (var == 2) s->mc_ftarget = (rnd() * 2 - 1) * 1.5f;
        spawn_g(s, r > 0 ? len * r + 4 : len + 4, r, r, len, (idx & 1) ? 0.4f : -0.4f, 0.75f, atk, 0, 0);
        s->mc_next += step;
        break;
    }
    case F_PATTERN: case F_WARP: {   /* taps: delays laid out in a pattern (Activity: how many), each a stream of grains */
        static const float PAT[4][8] = {{0.5f, 1, 1.5f, 2, 2.5f, 3, 3.5f, 4}, {0.75f, 1.5f, 2.25f, 3, 3.75f, 4.5f, 5.25f, 6},
                                        {0.333f, 0.667f, 1, 1.333f, 1.667f, 2, 2.333f, 2.667f}, {0.25f, 0.75f, 1, 1.5f, 1.75f, 2.5f, 2.75f, 3.5f}};
        static const float WT[4] = {0.25f, 0.5f, 0.75f, 1}, WR[4] = {2, 1.5f, 0.5f, 2};
        const int warp = fam == F_WARP, n = warp ? 4 : 1 + (int)(a * 7.99f);
        const float *D = warp ? WT : PAT[var];
        const float len = clampf(T * D[0], 0.01f * fs, 0.15f * fs);
        s->mc_step++;
        s->mc_g = 0;
        for (int k = 0; k < n; k++) {
            float r = 1;
            if (warp && var == 2 && rnd() < a) r = WR[k];
            if (warp && var == 3 && (s->mc_step & 1) && rnd() < a) r = 2;
            const float gk = warp ? 0.55f : powf(1 - (float)k / (float)(n + 1), lerp(0.3f, 2, sh)) * 0.9f / sqrtf((float)n);
            s->mc_g += gk;
            spawn_g(s, fminf(T * D[k], 9.5f * fs) + len * (r - 1) * 0.5f, r, r, len, (k & 1) ? 0.5f : -0.5f, gk, 0.5f, 0, 0);
        }
        s->mc_next += len * 0.5f;
        break;
    }
    default: s->mc_next += fs; break;
    }
}
static void micro_run(Plugin *s, const float in[2], float out[2]) {
    const float fs = s->fs, *v = s->v;
    const int rec = !s->hold, fx = (int)v[0], fam = fx >> 2, var = fx & 3;
    onsets(s, in, rec);
    const float fb = v[4] * 0.009f;
    if (rec) { line_push(&s->rec[0], in[0] + tanh_fast(s->mc_y[0] * fb)); line_push(&s->rec[1], in[1] + tanh_fast(s->mc_y[1] * fb)); }
    micro_events(s, rec);
    float o[2][2];
    run_grains_bus(s, rec, o);
    /* the filter */
    s->mc_fmod += 0.0015f * (s->mc_ftarget - s->mc_fmod);
    const int bp = (fam == F_TUNNEL && var == 2) || (fam == F_WARP && var == 1);
    const float fc = clampf(v[5] * exp2f(s->mc_fmod), 40, fs * 0.45f), gg = tanf(PI * fc / fs);
    const float reso = v[7] * 0.01f, k = bp ? 2 - 1.9f * fmaxf(reso, 0.6f) : 2 - 1.85f * reso;
    float y[2];
    for (int c = 0; c < 2; c++) y[c] = bp ? svf_bp(&s->svf[c], o[0][c], gg, k) * (1.8f * k + 0.2f) : svf_lp(&s->svf[c], o[0][c], gg, k);
    /* bit-crush: 6 bits, held for a few samples */
    if (var == 3 && (fam == F_SEQ || fam == F_BLOCKS || fam == F_INTERRUPT || fam == F_ARP)) {
        if ((s->crush_ph += 1) >= 4) { s->crush_ph = 0; for (int c = 0; c < 2; c++) s->crush_v[c] = roundf(clampf(y[c], -2, 2) * 32) / 32; }
        y[0] = s->crush_v[0]; y[1] = s->crush_v[1];
    }
    const float fbn = (bp ? 1 / (1.8f + 0.2f / k) : fminf(1, k)) / fmaxf(1, s->mc_g);   /* (the resonance's peak, the grains' sum) */
    s->mc_y[0] = y[0] * fbn; s->mc_y[1] = y[1] * fbn;
    float vl, vr;
    const float sp = v[6] * 0.01f, diff = fam == F_HAZE && var == 0 ? 0.4f : 0;
    verb_run(&s->verb, y[0] * (1 + diff), y[1] * (1 + diff), &vl, &vr);
    out[0] = y[0] * (1 - 0.4f * sp) + vl * (sp + diff) * 1.5f;
    out[1] = y[1] * (1 - 0.4f * sp) + vr * (sp + diff) * 1.5f;
    if (fam == F_INTERRUPT) {   /* the playing passes, except in a glitch */
        s->mc_gl += 0.004f * ((s->mc_glitch > 0 ? 1.0f : 0.0f) - s->mc_gl);
        out[0] += in[0] * (1 - s->mc_gl); out[1] += in[1] * (1 - s->mc_gl);
    }
}

/* ==== Moodd MK2: a wet channel and a micro-looper side by side ==============================================
 * Wet: Reverb (Time = decay, Modify = size, darker as it grows), Delay (Time = 40 ms .. 1.5 s, Modify = feedback),
 * Slip (grains of what you just played at a speed: Modify picks -2x -1x -0.5x 0.5x 1x 2x; Time = grain size). It
 * hears the input and half the loop. Loop: always listening to the last Length; Env plays it ducked under your
 * playing (Loop Mod = how much), Tape at a speed (Loop Mod across -2x .. 2x), Stretch slowed without changing pitch
 * (Loop Mod = how slow). Hold keeps the loop. Clock lowers the sample rate and bits; Balance wet <-> loop. */
static void mood_run(Plugin *s, const float in[2], float out[2]) {
    const float fs = s->fs, ms = fs * 0.001f, *v = s->v, *g = s->g;
    const int rec = !s->hold, wet = (int)v[0], lmode = (int)v[3];
    const float L = v[4] * fs, lm = v[5] * 0.01f, t = v[1] * 0.01f, m = v[2] * 0.01f;
    if (rec) { line_push(&s->rec[0], in[0]); line_push(&s->rec[1], in[1]); s->md_anchor += 1; }
    /* the loop */
    const float speed = lmode == 1 ? MOOD_SPEED_V[(int)(lm * 5.99f)] : 1, adv = lmode == 2 ? lerp(1, 0.08f, lm) : speed;
    s->md_pos += adv;
    if (s->md_pos >= L || s->md_pos < 0) { s->md_pos = s->md_pos >= L ? s->md_pos - L : s->md_pos + L; if (rec) s->md_anchor = 0; }
    s->md_pos = clampf(s->md_pos, 0, L);
    const float glen = (lmode == 2 ? 0.08f : 0.12f) * fs;
    if ((s->md_next -= 1) <= 0) {
        const float r = lmode == 2 ? 1 : speed;
        spawn_g(s, s->md_anchor + L - s->md_pos, r, r, glen, (rnd() - 0.5f) * 0.3f, 0.6f, 0.5f, 0, 1);
        s->md_next += glen * (lmode == 2 ? 0.25f : 0.5f);
    }
    /* the wet channel's Slip grains */
    if (wet == 2 && (s->md_snext -= 1) <= 0) {
        const float r = MOOD_SPEED_V[(int)(m * 5.99f)], sg = expmap(0.03f, 0.5f, t) * fs;
        spawn_g(s, r > 0 ? sg * r + 4 + rnd() * 0.02f * fs : sg + 4, r, r, sg, (rnd() - 0.5f) * 0.6f, 0.75f, 0.5f, 0, 0);
        s->md_snext += sg * 0.5f;
    }
    float o[2][2];
    run_grains_bus(s, rec, o);
    s->env += (fabsf(in[0]) + fabsf(in[1]) > s->env ? 0.005f : 0.0003f) * (fabsf(in[0]) + fabsf(in[1]) - s->env);
    const float lg = lmode == 0 ? 1 - lm * clampf(s->env * 6, 0, 1) : 1;
    const float loop[2] = {o[1][0] * lg, o[1][1] * lg};
    float w[2];
    const float x[2] = {in[0] + loop[0] * 0.5f, in[1] + loop[1] * 0.5f};
    if (wet == 0) {
        float vl, vr;
        verb_run(&s->verb, x[0], x[1], &vl, &vr);
        w[0] = vl * 0.9f; w[1] = vr * 0.9f;
    } else if (wet == 1) {
        s->dly_s[0] += 0.0005f * (expmap(40, 1500, t) * ms - s->dly_s[0]);
        const float dl = line_read(&s->dl[0], s->dly_s[0]), dr = line_read(&s->dl[1], s->dly_s[0] * 1.02f), f = m * 0.95f;
        line_push(&s->dl[0], x[0] + tanh_fast(lp(&s->op[0][1], dr, g[0]) * f));
        line_push(&s->dl[1], x[1] + tanh_fast(lp(&s->op[1][1], dl, g[0]) * f));
        w[0] = dl; w[1] = dr;
    } else { w[0] = o[0][0]; w[1] = o[0][1]; }
    /* Balance, then Clock */
    const float b = v[7] * 0.01f, gw = fminf(1, 2 * (1 - b)), gl = fminf(1, 2 * b);
    float y[2] = {w[0] * gw + loop[0] * gl, w[1] * gw + loop[1] * gl};
    const float clock = v[6] * 0.01f;
    if (clock < 0.99f) {
        const float hold = 1 + (1 - clock) * 11, q = exp2f(lerp(4, 12, clock));
        if ((s->crush_ph += 1) >= hold) { s->crush_ph -= hold; for (int c = 0; c < 2; c++) s->crush_v[c] = roundf(clampf(y[c], -2, 2) * q) / q; }
        for (int c = 0; c < 2; c++) y[c] = s->md_lp[c] += lp_coef(lerp(3000, 16000, clock), fs) * (s->crush_v[c] - s->md_lp[c]);
    }
    out[0] = y[0]; out[1] = y[1];
}

static void run_machine(Plugin *s, const float in[2], float out[2]) {
    const float fs = s->fs, ms = fs * 0.001f, *v = s->v, *g = s->g;
    const int hold = s->hold;
    switch (s->machine) {
    case MK_MICRO: micro_run(s, in, out); break;
    case MK_MOOD: mood_run(s, in, out); break;
    case MK_PARTICLE: {
        const float fb = v[6] * 0.009f;
        if (!hold) {
            line_push(&s->rec[0], in[0] + tanh_fast(s->st[0][1] * fb));
            line_push(&s->rec[1], in[1] + tanh_fast(s->st[1][1] * fb));
        }
        /* Mode: Random (scattered in time by Spread), Density (evenly spaced), LFO (the delay swept), Rnd Pitch
         * (each grain at unison, Pitch or its inverse), Reverse, Detune (each grain a little off, Spread = how
         * much), Dns Pitch (grains climbing unison, Pitch, twice Pitch), LFO Pitch (pitch swept 0 .. Pitch) */
        const int mode = (int)v[4];
        const float glen = v[1] * ms, ratio = exp2f(v[3] / 12), sp = v[5] * 0.01f;
        s->lfo[0] += lerp(0.05f, 2, sp) / fs; s->lfo[0] -= floorf(s->lfo[0]);
        s->timer -= v[2] / fs;
        if (s->timer <= 0) {
            s->timer += mode == 1 ? 1 : 0.5f + rnd();
            float rate = ratio, d = v[0] * ms;
            switch (mode) {
            case 0: d *= 1 + (rnd() - 0.5f) * sp * 1.2f; break;
            case 1: d *= 1 + (rnd() - 0.5f) * 0.02f; break;
            case 2: d *= 1 + 0.5f * sp * sine01(s->lfo[0]); break;
            case 3: { const float c = rnd(); rate = c < 0.34f ? 1 : c < 0.67f ? ratio : 1 / ratio; d *= 1 + (rnd() - 0.5f) * sp * 0.6f; break; }
            case 4: rate = -ratio; d *= 1 + (rnd() - 0.5f) * sp * 0.6f; break;
            case 5: rate = ratio * exp2f((rnd() - 0.5f) * sp * 0.5f / 12); d *= 1 + (rnd() - 0.5f) * 0.2f; break;
            case 6: s->st[0][2] = (float)(((int)s->st[0][2] + 1) % 3); rate = powf(ratio, s->st[0][2]); break;
            default: rate = powf(ratio, 0.5f + 0.5f * sine01(s->lfo[0])); break;
            }
            spawn(s, d + glen * (rate > 1 ? rate : 1) + 4, rate, glen, (rnd() * 2 - 1) * 0.7f, 1 / sqrtf(1 + v[2] * v[1] * 0.001f));
        }
        float l, r;
        run_grains(s, !hold, &l, &r);
        l = lp(&s->op[0][0], l, g[0]); r = lp(&s->op[1][0], r, g[0]);
        s->st[0][1] = l; s->st[1][1] = r;
        out[0] = l * 1.5f; out[1] = r * 1.5f;
        break;
    }
    case MK_TENSOR: {
        if (!hold) { line_push(&s->rec[0], in[0]); line_push(&s->rec[1], in[1]); }
        const float L = v[4] * fs, glen = 0.07f * fs, speed = v[0], stretch = v[1];
        const float wow = 1 + 0.002f * (sine01(s->lfo[0]) + 0.3f * sine01(s->lfo[1]));   /* (a little tape wow) */
        const int dir = (int)v[5];
        s->lfo[0] += 0.6f / fs; s->lfo[0] -= floorf(s->lfo[0]);
        s->lfo[1] += 7.0f / fs; s->lfo[1] -= floorf(s->lfo[1]);
        /* the tape head: moves through the recent past at Speed x Stretch (the grains play at Speed x Pitch) */
        float *head = &s->st[0][2];
        if (*head < glen * 3) *head = glen * 3;
        *head += (hold ? 0.0f : 1.0f) - speed * stretch;
        if (*head > L) *head = glen * 3 + 4;
        if (*head < glen * 3) *head = L;
        /* stutter: now and then the head comes back to where it was a moment ago, a few times over */
        s->st[0][3] -= v[2] / fs;
        if (s->st[0][3] <= 0) {
            s->st[0][3] += 1;
            if (rnd() * 100 < v[3]) { s->st[0][4] = *head; s->st[0][5] = 1; } else s->st[0][5] = 0;
        }
        const float d0 = s->st[0][5] > 0.5f ? s->st[0][4] : *head;
        s->timer -= 1;
        if (s->timer <= 0) {
            s->timer = glen * 0.5f;
            s->st[0][6] = 1 - s->st[0][6];   /* (Alt: every other grain backwards) */
            const float rate = speed * exp2f(v[6] / 12) * wow * (dir == 1 || (dir == 2 && s->st[0][6] > 0.5f) ? -1 : 1);
            spawn(s, d0 + glen * (rate > 1 ? rate : 1), rate, glen, 0, 0.7f);
        }
        float l, r;
        run_grains(s, !hold, &l, &r);
        out[0] = lp(&s->op[0][0], l, g[0]); out[1] = lp(&s->op[1][0], r, g[0]);
        break;
    }
    case MK_NIGHT: {
        /* the step sequencer moves the filter in octaves; the shimmer pitch-shifts the tail back into itself */
        s->lfo[0] += v[7] / fs;
        if (s->lfo[0] >= 1) { s->lfo[0] -= 1; s->st[0][6] = (float)(((int)s->st[0][6] + 1) & 7); }
        const float step = s->seq[(int)s->st[0][6]] * v[6] * 0.01f * 2;   /* +-2 octaves */
        s->st[0][7] += 0.004f * (step - s->st[0][7]);
        const float fc = clampf(v[4] * exp2f(s->st[0][7]), 60, fs * 0.4f), gg = tanf(PI * fc / fs), k = 2 - 1.9f * v[5] * 0.01f;
        const float shim = v[2] * 0.01f;
        float x[2];
        for (int c = 0; c < 2; c++) {
            const float sh_in = shift(&s->sh[c][0], s->st[c][8], exp2f(SHIM_ST[(int)v[3]] / 12) * (c ? 1.003f : 1), 0.05f * fs);
            x[c] = svf_lp(&s->svf[c], in[c] * (hold ? 0.02f : 1) + tanh_fast(sh_in * shim * 0.9f * g[2] * 4), gg, k) * (0.3f + 0.35f * k);
        }
        float vl, vr;
        verb_run(&s->verb, x[0], x[1], &vl, &vr);
        s->st[0][8] = vl; s->st[1][8] = vr;
        out[0] = vl * 1.5f; out[1] = vr * 1.5f;
        break;
    }
    case MK_CLOUD: {
        /* swell: the reverb hears less while you play, so it blooms after; then an ensemble over the tail */
        const float lvl = fmaxf(fabsf(in[0]), fabsf(in[1]));
        s->env += (lvl > s->env ? 0.005f : 0.0002f) * (lvl - s->env);
        const float gin = (hold ? 0.02f : 1) / (1 + v[3] * 0.01f * s->env * 20);
        float x[2] = {0, 0};
        for (int c = 0; c < 2; c++) { line_push(&s->dl[c], in[c] * gin); x[c] = line_read(&s->dl[c], v[4] * ms); }
        float vl, vr;
        verb_run(&s->verb, x[0], x[1], &vl, &vr);
        vl += lp(&s->op[0][1], vl, g[0]) * g[1];
        vr += lp(&s->op[1][1], vr, g[0]) * g[1];
        float y[2] = {vl, vr};
        const float ens = v[2] * 0.01f, mod = v[7] * 0.01f;
        for (int c = 0; c < 2; c++) {
            line_push(&s->dl2[c], y[c]);
            float e = 0;
            for (int k = 0; k < 3; k++) {
                const float ph = s->lfo[k] + (float)c * 0.25f;
                e += line_read(&s->dl2[c], (8 + 4 * (float)k + (2 + 4 * mod) * (1 + sine01(ph))) * ms);
            }
            out[c] = (y[c] * (1 - 0.5f * ens) + e * 0.33f * ens) * 1.6f;
        }
        for (int k = 0; k < 3; k++) { s->lfo[k] += (0.13f + 0.07f * (float)k) * (0.5f + mod) / fs; s->lfo[k] -= floorf(s->lfo[k]); }
        break;
    }
    case MK_POLY: {
        const int dim = (int)v[2];
        const float t = v[0] * ms, depth = v[3] * 0.01f * 0.004f * fs;
        s->lfo[0] += v[4] / fs; s->lfo[0] -= floorf(s->lfo[0]);
        s->lfo[1] += v[4] * 0.37f / fs; s->lfo[1] -= floorf(s->lfo[1]);
        s->dly_s[0] += 0.0005f * (t - s->dly_s[0]);
        for (int c = 0; c < 2; c++) {
            float sum = 0;
            for (int k = 1; k <= dim; k++) {   /* the repeat spread into Dimension taps, each swaying its own way */
                const float d = s->dly_s[0] * (0.4f + 0.6f * (float)k / (float)dim) + depth * (1 + sine01(s->lfo[0] + 0.17f * (float)k + 0.25f * (float)c));
                sum += line_read(&s->dl[c], d);
            }
            const float fbsum = sum / (float)dim;   /* (fed back: the taps can add up in phase, at the lows most) */
            sum /= sqrtf((float)dim);
            /* a phaser (four swept allpasses) and a flanger (a short swept delay) inside the repeats */
            const float a = lerp(-0.2f, 0.85f, 0.5f + 0.5f * sine01(s->lfo[1] + 0.3f * (float)c));
            float ph2 = fbsum;
            for (int k = 0; k < 4; k++) { const float yy = -a * ph2 + s->st[c][k]; s->st[c][k] = ph2 + a * yy; ph2 = yy; }
            float y = lerp(fbsum, (fbsum + ph2) * 0.5f, v[5] * 0.01f);   /* (both mixes peak at unity: the loop stays under one) */
            line_push(&s->dl2[c], y);
            const float fl = line_read(&s->dl2[c], (0.5f + 3 * (1 + sine01(s->lfo[1] * 0.7f + 0.5f * (float)c))) * ms);
            y = lerp(y, (y + fl) * 0.5f, v[6] * 0.01f);
            y = lp(&s->op[c][0], y, g[0]);
            y -= lp(&s->op[c][1], y, g[1]);   /* (no DC round the loop) */
            line_push(&s->dl[c], in[c] + tanh_fast(y * v[1] * 0.0097f));
            out[c] = sum;
        }
        break;
    }
    case MK_HEDRA: {
        static const float RH[5][3] = {{1, 2, 3}, {0.6667f, 1.3333f, 2}, {0.75f, 1.5f, 2.25f}, {1, 1.6667f, 2.6667f}, {1, 2, 3}};
        const int rh = (int)v[4], key = (int)v[6], scale = (int)v[7] - 1;   /* (scale -1: Off, plain semitones) */
        const float t = v[3] * ms, mono = (in[0] + in[1]) * 0.5f;
        s->dly_s[0] += 0.0005f * (t - s->dly_s[0]);
        const float xin = mono + tanh_fast(s->st[0][9] * v[5] * 0.009f);
        line_push(&s->dl[0], xin);
        if (scale >= 0) {   /* in key: the pitch going into the line, remembered so each voice knows what it reads */
            track(&s->trk, xin, fs);
            if (++s->hd_cnt >= 512) {
                s->hd_cnt = 0;
                s->hd_w = (s->hd_w + 1) & 1023;
                s->hd_m[s->hd_w] = s->trk.conf > 0.6f && s->trk.hz > 50 && s->trk.hz < 1500 ? midi_of(s->trk.hz) : -1;
            }
        }
        float l = 0, r = 0, fbs = 0, cum = 0;
        static const float DET[3] = {0.07f, -0.07f, 0.035f};   /* (a voice at unison is detuned a little: a chorus) */
        for (int k = 0; k < 3; k++) {
            const float d = clampf(s->dly_s[0] * RH[rh][k], 2, (float)s->dl[0].mask - 4);
            cum = rh == 4 ? cum + v[k] : v[k];   /* Cascade: each voice stacks on the last */
            float st = clampf(cum, -36, 36) + (cum == 0 && scale < 0 ? DET[k] : 0);
            if (scale >= 0) {   /* the voice lands on the scale note nearest the interval (pitch correction) */
                const float m = s->hd_m[(s->hd_w - (int)(d / 512) + 1024) & 1023];
                if (m > 0) st = nearest_in_scale(roundf(m) + st - (st > 0 ? 0.01f : st < 0 ? -0.01f : 0), key, scale) - m;   /* (a tie goes toward the input) */
            }
            s->hd_ratio[k] += 0.003f * (exp2f(st / 12) - s->hd_ratio[k]);
            const float y = shift(&s->sh[0][k], line_read(&s->dl[0], d), s->hd_ratio[k], 0.05f * fs);
            const float pan = (k == 0 ? -1.0f : k == 1 ? 1.0f : 0.0f) * 0.7f;
            l += y * (1 - pan) * 0.6f; r += y * (1 + pan) * 0.6f;
            fbs += y;
        }
        s->st[0][9] = lp(&s->op[0][0], fbs * 0.4f, g[0]);
        out[0] = lp(&s->op[0][1], l, g[0]); out[1] = lp(&s->op[1][1], r, g[0]);
        break;
    }
    case MK_AFTER: {
        const int n = (int)v[7];
        const float span = v[0] * ms, drag = v[2] * 0.01f, mod = v[4] * 0.01f;
        s->lfo[0] += 0.23f / fs; s->lfo[0] -= floorf(s->lfo[0]);
        for (int c = 0; c < 2; c++) {
            float sum = 0, wsum = 0;
            for (int k = 0; k < n; k++) {   /* the swarm: taps over the span, bunched by Drag, wandering by Mod */
                const float pos = (float)(k + 1) / (float)n, bunched = powf(pos, 1 + 2.5f * drag);
                const float d = 1 + span * bunched * (c ? 0.93f : 1) + mod * 0.003f * fs * (1 + sine01(s->lfo[0] + 0.13f * (float)k + 0.4f * (float)c));
                sum += line_read(&s->dl2[c], clampf(d, 1, (float)s->dl2[c].mask - 4)) * (1 - 0.5f * pos);
                wsum += 1 - 0.5f * pos;
            }
            sum /= wsum;   /* (the taps' weights sum to one: the loop's gain is Reflect at most) */
            float y = sum;
            for (int k = 0; k < 6; k++) y = allpass(&s->ap[c][k], s->apd[c][k], y, v[1] * 0.0072f);
            y = lp(&s->op[c][0], y, g[0]);
            line_push(&s->dl2[c], in[c] + tanh_fast(y * v[3] * 0.0094f));
            s->st[c][10] = y;
        }
        const float w = v[6] * 0.01f, mid = (s->st[0][10] + s->st[1][10]) * 0.5f;
        out[0] = lerp(mid, s->st[0][10], w) * 3; out[1] = lerp(mid, s->st[1][10], w) * 3;
        break;
    }
    case MK_RAINBOW: {
        /* tracking: the pitch wanders to a new wrong place every so often, by up to a semitone */
        s->st[0][11] -= 1;
        if (s->st[0][11] <= 0) { s->st[0][11] = (0.05f + 0.25f * rnd()) * fs; s->st[0][12] = (rnd() * 2 - 1) * v[2] * 0.01f; }
        s->st[0][13] += 0.0008f * (s->st[0][12] - s->st[0][13]);
        s->lfo[0] += v[5] / fs; s->lfo[0] -= floorf(s->lfo[0]);
        const float cents = s->st[0][13] * 100 + v[4] * 0.6f * sine01(s->lfo[0]);
        const float ratio = exp2f((v[0] + cents * 0.01f) / 12), r2 = exp2f((SECOND_ST[(int)v[1]] + cents * 0.007f) / 12);
        for (int c = 0; c < 2; c++) {
            /* Magic: the shifted sound shifted again (two voices add up, so less of it goes round then) */
            const float fb = line_read(&s->dl[c], 0.09f * fs) * v[3] * 0.0085f / ((int)v[1] > 0 ? 1.4f : 1.0f);
            const float x = in[c] + tanh_fast(fb);
            float y = shift(&s->sh[c][0], x, ratio * (c ? 1.002f : 1), 0.045f * fs);
            if ((int)v[1] > 0) y = y * 0.7f + shift(&s->sh[c][1], x, r2, 0.05f * fs) * 0.6f;
            y = lp(&s->op[c][0], y, g[0]);
            line_push(&s->dl[c], y);
            out[c] = y;
        }
        break;
    }
    case MK_DATA: {
        const float mono = (in[0] + in[1]) * 0.5f;
        track(&s->trk, mono, fs);
        const float a = fabsf(mono);
        s->env += (a > s->env ? 0.01f : 0.0004f) * (a - s->env);
        const int voiced = s->trk.conf > 0.7f && s->trk.hz > 40 && s->trk.hz < 2000;
        const float target = voiced ? s->trk.hz * exp2f(v[0] / 12) : s->st[0][14];
        float *f = &s->st[0][14];
        if (*f <= 0) *f = target > 0 ? target : 110;
        *f += (1 - expf(-1 / (v[5] * ms))) * (target - *f);
        s->lfo[0] += 5.5f / fs; s->lfo[0] -= floorf(s->lfo[0]);
        const float hz = *f * exp2f(v[6] * 0.005f * sine01(s->lfo[0]) / 12);
        s->lfo[1] += hz / fs; s->lfo[1] -= floorf(s->lfo[1]);
        s->lfo[2] += hz / ((float)v[1] + 2) / fs; s->lfo[2] -= floorf(s->lfo[2]);   /* the subharmonic: /2 .. /5 */
        const float sq = s->lfo[1] < 0.5f ? 1.0f : -1.0f, sub = s->lfo[2] < 0.5f ? 1.0f : -1.0f;
        s->st[0][15] += 0.003f * ((voiced ? 1.0f : 0.0f) - s->st[0][15]);
        const float gate = s->st[0][15] * clampf(s->env * 6, 0, 1);
        const float synth = (sq * v[2] + sub * v[3]) * 0.01f * gate * 0.3f;
        for (int c = 0; c < 2; c++) {
            const float fz = tanh_fast(in[c] * 40) * 0.25f * v[4] * 0.01f;
            out[c] = lp(&s->op[c][0], lp(&s->op[c][1], synth + fz, g[0]), g[0]);
        }
        break;
    }
    default: out[0] = in[0]; out[1] = in[1]; break;
    }
}

static void load_defaults(Plugin *s, int m) {
    for (int i = 0; i < NK; i++) {
        const Knob *k = &MACHINES[m].k[i];
        s->norm[kK + i] = k->name ? knob_norm(k, k->def) : 0;
    }
    s->norm[kMix] = MACHINES[m].mix * 0.01f;
    s->automate |= ((1u << NK) - 1) << kK | 1u << kMix | 1u << kMachine;
    s->update_display = 1;
}
static void set_machine(Plugin *s, int m) {
    s->norm[kMachine] = (float)m / (float)(kNumMachines - 1);
    load_defaults(s, m);
}
static void load_preset(Plugin *s, int idx) {
    const Preset *p = &PRESETS[idx];
    set_machine(s, p->machine);
    for (int i = 0; i < NK; i++) {
        const Knob *k = &MACHINES[p->machine].k[i];
        if (k->name) s->norm[kK + i] = knob_norm(k, p->k[i]);
    }
    s->norm[kMix] = p->mix * 0.01f;
    s->preset = idx;
    s->norm[kPreset] = (float)idx / (float)(NPRESETS - 1);
    s->automate |= 1u << kPreset;
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
    VC_FLUSH_DENORMALS_BEGIN;
    Plugin *s = (Plugin *)e->object;
    if (s->master) {
        const uint32_t a = s->automate;
        if (a) {
            s->automate &= ~a;
            for (int i = 0; i < kNumParams; i++) if (a & (1u << i)) s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]);
            s->update_display = 1;
        }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    {
        const int m = option_of(s->norm[kMachine], kNumMachines);
        if (m != s->machine) { s->machine = m; reset(s); }
        const int hold = s->norm[kHold] > 0.5f;
        if (hold != s->hold) { s->hold = hold; s->tick = 0; }
        const float og = db(lerp(-24, 12, s->norm[kOutput]));
        for (int k = 0; k < n; k++) {
            if ((s->tick++ & 15) == 0) control(s);
            float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
            if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
            if (!finite_bits(r) || r > 8 || r < -8) r = 0;
            const float x[2] = {l, r};
            float w[2];
            run_machine(s, x, w);
            s->mix_s += 0.002f * (s->norm[kMix] - s->mix_s);
            const float mx = s->mix_s, dg = mx < 0.5f ? 1 : 2 * (1 - mx), wg = mx > 0.5f ? 1 : 2 * mx;
            s->out_s += 0.002f * (og - s->out_s);
            float yl = (l * dg + w[0] * wg) * s->out_s, yr = (r * dg + w[1] * wg) * s->out_s;
            if (!finite_bits(yl) || !finite_bits(yr) || fabsf(yl) > 63 || fabsf(yr) > 63) { reset(s); yl = yr = 0; }   /* past +36 dB: start over */
            out[0][k] = clampf(yl, -4, 4);   /* +12 dB ceiling */
            out[1][k] = clampf(yr, -4, 4);
        }
    }
done:
    VC_FLUSH_DENORMALS_END;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    if (i == kPresetPrev || i == kPresetNext) {   /* < > step through the machine's own presets */
        const int m = option_of(s->norm[kMachine], kNumMachines), n = presets_in(m);
        if (v > 0.5f && n > 0) {
            const int cur = s->preset >= 0 && s->preset < NPRESETS && PRESETS[s->preset].machine == m ? place_of(s->preset) : -1;
            load_preset(s, preset_of(m, cur < 0 ? (i == kPresetNext ? 0 : n - 1) : (cur + (i == kPresetNext ? 1 : -1) + n) % n));
        }
        s->norm[i] = 0;
        s->automate |= 1u << i;
        return;
    }
    if (i == kPreset) {   /* a small move (a Q-Link nudge) steps one; an exact value (a project loading) only names it */
        const float pos = v * (float)(NPRESETS - 1), cur = s->preset >= 0 ? (float)s->preset : s->norm[i] * (float)(NPRESETS - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) { setParameter(e, pos > cur ? kPresetNext : kPresetPrev, 1); return; }
        s->preset = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    if (i == kMachine) {   /* a pick from the list (closes it), or a Q-Link nudge (steps one) */
        const float pos = v * (float)(kNumMachines - 1), cur = (float)option_of(s->norm[kMachine], kNumMachines);
        const int between = fabsf(pos - roundf(pos)) > 0.001f;
        int opt = between && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
        opt = opt < 0 ? 0 : opt > kNumMachines - 1 ? kNumMachines - 1 : opt;
        if (opt != (int)cur) set_machine(s, opt);
        else s->norm[i] = (float)opt / (float)(kNumMachines - 1);
        if (!between && s->norm[kMachineOpen] > 0.5f) { s->norm[kMachineOpen] = 0; s->automate |= 1u << kMachineOpen; }
        s->update_display = 1;
        return;
    }
    if (i == kMachineOpen || i == kHold) { s->norm[i] = v > 0.5f ? 1 : 0; s->update_display = 1; return; }
    if (i >= kK && i < kK + NK) {   /* a choice or whole-number knob snaps (a Q-Link nudge steps one) */
        const Knob *k = &MACHINES[option_of(s->norm[kMachine], kNumMachines)].k[i - kK];
        if (k->name && (k->map == M_CHOICE || k->map == M_INT)) {
            const int nn = (int)(k->hi - k->lo) + 1;
            const float pos = v * (float)(nn - 1), cur = (float)option_of(s->norm[i], nn);
            int opt = nn > 2 && fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
            opt = opt < 0 ? 0 : opt > nn - 1 ? nn - 1 : opt;
            v = (float)opt / (float)(nn - 1);
            s->update_display = 1;
        }
    }
    s->norm[i] = v;
}
static float getParameter(AEffect *e, int32_t i) { Plugin *s = (Plugin *)e->object; return i >= 0 && i < kNumParams ? s->norm[i] : 0; }

static void fmt_knob(const Knob *k, float n, char *t) {
    if (!k->name) { snprintf(t, 32, " "); return; }
    const float v = knob_value(k, n);
    if (k->map == M_CHOICE) { snprintf(t, 32, "%s", k->choices[(int)v]); return; }
    if (!strcmp(k->unit, "ms") && v >= 1000) { snprintf(t, 32, "%.2f s", v / 1000); return; }
    if (!strcmp(k->unit, "Hz") && v >= 1000) { snprintf(t, 32, "%.1f kHz", v / 1000); return; }
    if (!strcmp(k->unit, "st")) { snprintf(t, 32, "%+.0f st", v); return; }
    if (!strcmp(k->unit, "dB")) { snprintf(t, 32, "%+.1f dB", v); return; }
    if (!strcmp(k->unit, "x")) { snprintf(t, 32, "%.2fx", v); return; }
    const int pct = !strcmp(k->unit, "%");
    if (k->map == M_INT || pct || fabsf(v) >= 100) snprintf(t, 32, "%.0f%s%s", v, k->unit[0] && !pct ? " " : "", k->unit);
    else snprintf(t, 32, fabsf(v) >= 10 ? "%.1f %s" : "%.2f %s", v, k->unit);
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: free_all(s); free(s); return 1;
    case effGetParamName:
        if (idx >= 0 && idx < kNumParams) {
            static const char *const N[] = {"Machine", "Machine List", "Mix"};
            char t[40];
            if (idx < kK) snprintf(t, sizeof t, "%s", N[idx]);
            else if (idx < kK + NK) { const Knob *k = &MACHINES[option_of(s->norm[kMachine], kNumMachines)].k[idx - kK]; snprintf(t, sizeof t, "%s", k->name ? k->name : "-"); }
            else snprintf(t, sizeof t, "%s", idx == kHold ? "Hold" : idx == kPreset ? "Preset" : idx == kPresetPrev ? "Preset <" : idx == kPresetNext ? "Preset >" : "Output");
            copy_str(p, t, 32);
        }
        return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay:
        if (idx >= 0 && idx < kNumParams) {
            char t[64];
            const int m = option_of(s->norm[kMachine], kNumMachines);
            if (idx == kMachine) snprintf(t, sizeof t, "%s", MACHINES[m].name);
            else if (idx == kMachineOpen) snprintf(t, sizeof t, "%s", s->norm[idx] > 0.5f ? "Open" : "Closed");
            else if (idx == kMix) snprintf(t, sizeof t, "%.0f%%", s->norm[idx] * 100);
            else if (idx < kK + NK) fmt_knob(&MACHINES[m].k[idx - kK], s->norm[idx], t);
            else if (idx == kHold) snprintf(t, sizeof t, "%s", s->norm[idx] > 0.5f ? "Held" : "Off");
            else if (idx == kPreset) snprintf(t, sizeof t, "%s", s->preset >= 0 && s->preset < NPRESETS && PRESETS[s->preset].machine == m ? PRESETS[s->preset].name : "-");
            else if (idx == kPresetPrev || idx == kPresetNext) snprintf(t, sizeof t, "%s", idx == kPresetPrev ? "<" : ">");
            else snprintf(t, sizeof t, "%+.1f dB", lerp(-24, 12, s->norm[idx]));
            copy_str(p, t, 24);
        }
        return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams;
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Ambient Lab", 32); return 1;
    case effGetVendorString: copy_str(p, VC_VENDOR, 32); return 1;
    case effGetVendorVersion: return 1000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) reset(s); return 1;
    case effProcessEvents: {   /* MIDI Program Change 0-14: the machine's preset 1-15; 16-26: the machine; CC 64 (sustain): Hold */
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents; k++) {
            if (!ev->events[k] || ev->events[k]->type != 1) continue;
            const unsigned char *md = ev->events[k]->midiData;
            if ((md[0] & 0xf0) == 0xc0 && md[1] >= 16 && md[1] < 16 + kNumMachines) set_machine(s, md[1] - 16);
            else if ((md[0] & 0xf0) == 0xc0) {
                const int idx = preset_of(option_of(s->norm[kMachine], kNumMachines), md[1]);
                if (idx >= 0) load_preset(s, idx);
            }
            else if ((md[0] & 0xf0) == 0xb0 && md[1] == 64) { s->norm[kHold] = md[2] >= 64 ? 1 : 0; s->automate |= 1u << kHold; }
        }
        return 1;
    }
    case effCanDo:
        return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

/* the colour of the page after the pedal the machine is like (tools/pedal_looks.h; for Quad Rig's slots) */
#include "../../tools/pedal_looks.h"
int amb_look(AEffect *e) {
    static const unsigned char LOOK[kNumMachines] = {LK_SILVER, LK_TURQUOISE, LK_ORANGE, LK_RED, LK_PURPLE, LK_SKY, LK_BLACK,
                                                     LK_YELLOW, LK_TEAL, LK_MAGENTA, LK_COPPER};
    return LOOK[option_of(((Plugin *)e->object)->norm[kMachine], kNumMachines)];
}

/* the full-page lists (tools/picklist.h; Quad Rig calls this for its slots' engines too): the presets, then the
 * eight knobs (lists 1..8: a knob that is a choice lists its choices; any other doesn't open) */
int amb_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    if (list >= 1) {
        const int idx = kK + list - 1;
        const Knob *kn = &MACHINES[option_of(s->norm[kMachine], kNumMachines)].k[list - 1];
        if (!kn->name || kn->map != M_CHOICE) return op == PL_COUNT ? -1 : op == PL_CURRENT ? -1 : 0;
        const int n = (int)(kn->hi - kn->lo) + 1;
        if (op == PL_COUNT) return n;
        if (op == PL_CURRENT) return option_of(s->norm[idx], n);
        if (op == PL_HEADING) { snprintf(out, (size_t)max, "%s", kn->name); return 0; }
        if (i < 0 || i >= n) return 0;
        if (op == PL_NAME) snprintf(out, (size_t)max, "%s", kn->choices[i]);
        else if (op == PL_CHOOSE) {
            s->norm[idx] = n > 1 ? (float)i / (float)(n - 1) : 0;
            s->automate |= 1u << idx;
            s->update_display = 1;
        }
        return 0;
    }
    /* the presets: the machine's own */
    const int m = option_of(s->norm[kMachine], kNumMachines);
    if (op == PL_HEADING) return 0;
    if (op == PL_COUNT) return presets_in(m);
    if (op == PL_CURRENT) return s->preset >= 0 && s->preset < NPRESETS && PRESETS[s->preset].machine == m ? place_of(s->preset) : -1;
    const int idx = preset_of(m, i);
    if (idx < 0) return 0;
    if (op == PL_NAME) snprintf(out, (size_t)max, "%s", PRESETS[idx].name);
    else if (op == PL_CHOOSE) load_preset(s, idx);
    return 0;
}
static const char *const LIST_KEYS[] = {"preset", "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8"};
static const char *const LIST_TITLES[] = {"Presets", "Knob 1", "Knob 2", "Knob 3", "Knob 4", "Knob 5", "Knob 6", "Knob 7", "Knob 8"};
static const PickLists LISTS = {9, LIST_KEYS, LIST_TITLES, amb_pick, 1};   /* (1: MIDI Ch) */

static AEffect *make(audioMasterCallback master) {
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    s->preset = -1;
    s->machine = -1;
    s->norm[kOutput] = 24.0f / 36;   /* 0 dB */
    set_machine(s, MK_MICRO);
    s->automate = 0;
    s->out_s = 1;
    for (int k = 0; k < 8; k++) s->seq[k] = rnd() * 2 - 1;   /* NiteSky's filter steps */
    setup(s, 44100);
    s->machine = MK_MICRO;
    AEffect *e = &s->fx;
    e->magic = VST_MAGIC;
    e->dispatcher = dispatcher;
    e->process = process;
    e->processReplacing = process;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->numParams = kNumParams;
    e->numInputs = e->numOutputs = 2;
    e->flags = effFlagsCanReplacing;
    e->object = s;
    e->uniqueID = 0x414d424c;   /* 'AMBL' */
    e->version = 1000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
#ifdef PICKLIST_OFF
    return make(master);
#else
    return pl_wrap(master, make, &LISTS);
#endif
}

#ifdef AMB_PARAMS
/* the page's parameter list (params.json), from the tables above */
int main(void) {
    printf("{\n  \"name\": \"Ambient Lab\",\n  \"params\": [\n");
    printf("    {\"key\": \"machine\", \"name\": \"Machine\", \"options\": [");
    for (int m = 0; m < kNumMachines; m++) printf("%s\"%s\"", m ? ", " : "", MACHINES[m].name);
    printf("]},\n");
    printf("    {\"key\": \"machine__open\", \"name\": \"Machine List\", \"options\": [\"Closed\", \"Open\"]},\n");
    printf("    {\"key\": \"mix\", \"name\": \"Mix\", \"min\": 0, \"max\": 100, \"unit\": \"%%\"},\n");
    for (int i = 0; i < NK; i++) printf("    {\"key\": \"k%d\", \"name\": \"Knob %d\", \"min\": 0, \"max\": 100},\n", i + 1, i + 1);
    printf("    {\"key\": \"hold\", \"name\": \"Hold\", \"options\": [\"Off\", \"Held\"]},\n");
    printf("    {\"key\": \"preset\", \"name\": \"Preset\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"},\n");
    printf("    {\"key\": \"preset_prev\", \"name\": \"Preset <\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n");
    printf("    {\"key\": \"preset_next\", \"name\": \"Preset >\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n");
    printf("    {\"key\": \"output\", \"name\": \"Output\", \"min\": -24, \"max\": 12, \"default\": 0, \"unit\": \"dB\"}");
    pl_params_json(&LISTS, stdout);
    printf("\n  ]\n}\n");
    return 0;
}
#endif
