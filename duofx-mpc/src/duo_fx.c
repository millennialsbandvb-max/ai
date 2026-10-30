/* Duo FX: an H90-style dual multi-effect for MPC OS (VST2, stereo). Two slots, A and B, in series (A then B) or
 * in parallel, each running one of 56 algorithms in six groups (delay, distortion, pitch, modulation, reverb,
 * granular), named after the H90 algorithms they imitate (near-miss spellings: this is a personal build). Each
 * algorithm has up to 8 knobs of its own (names, ranges, defaults in ALGOS[]); the slot's eight
 * knob parameters take those meanings, and eight read-only "name" parameters tell the page what each knob is.
 * Choosing an algorithm loads its defaults. All of the sound processing is our own.
 * Safety, as in every plugin here: input cleaned, everything reset if the output goes bad or past +36 dB, the
 * output capped at +12 dB (one NaN reaching MPC's mixer silences it until restart). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vst2.h"
#include "presets.h"
#include "../../tools/picklist.h"
#include "../../tools/pedal_looks.h"

#define PI 3.14159265358979f
#define NK 8                        /* knobs per slot */

/* ==== knob meanings ========================================================================================== */
enum { M_LIN, M_EXP, M_INT, M_CHOICE };
typedef struct { const char *name; int map; float lo, hi, def; const char *unit; const char *const *choices; } Knob;
#define LIN(n, lo, hi, d, u) {n, M_LIN, lo, hi, d, u, NULL}
#define EXPK(n, lo, hi, d, u) {n, M_EXP, lo, hi, d, u, NULL}
#define INT(n, lo, hi, d, u) {n, M_INT, lo, hi, d, u, NULL}
#define CHO(n, list, d) {n, M_CHOICE, 0, (float)(sizeof list / sizeof *list - 1), d, "", list}
#define PCT(n, d) LIN(n, 0, 100, d, "%")
#define MS(n, lo, hi, d) EXPK(n, lo, hi, d, "ms")
#define HZ(n, lo, hi, d) EXPK(n, lo, hi, d, "Hz")
#define ST(n, d) INT(n, -24, 24, d, "st")
#define NONE {NULL, 0, 0, 0, 0, NULL, NULL}

static const char *const KEYS[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const char *const SCALES[] = {"Major", "Minor", "Dorian", "Mixolydian", "Harm Minor", "Penta Maj", "Penta Min", "Blues"};
static const int SCALE_STEPS[8][8] = {   /* the count, then each degree's semitones */
    {7, 0, 2, 4, 5, 7, 9, 11}, {7, 0, 2, 3, 5, 7, 8, 10}, {7, 0, 2, 3, 5, 7, 9, 10}, {7, 0, 2, 4, 5, 7, 9, 10},
    {7, 0, 2, 3, 5, 7, 8, 11}, {5, 0, 2, 4, 7, 9}, {5, 0, 3, 5, 7, 10}, {6, 0, 3, 5, 6, 7, 10}};
static const char *const DEGREES[] = {"Oct Down", "7th Down", "6th Down", "5th Down", "4th Down", "3rd Down", "2nd Down",
                                      "Unison", "2nd Up", "3rd Up", "4th Up", "5th Up", "6th Up", "7th Up", "Oct Up"};
static const char *const ONOFF[] = {"Off", "On"};
static const char *const FTYPES[] = {"Lowpass", "Bandpass", "Highpass"};
static const char *const HEADS[] = {"1", "2", "3", "1+2", "1+3", "2+3", "1+2+3"};
static const char *const SHAPES[] = {"Sine", "Triangle", "Square", "Random"};
static const char *const STAGES[] = {"4", "6", "8", "12"};
static const char *const VIBE_MODES[] = {"Chorus", "Vibrato"};
static const char *const PHASE_MODES[] = {"Phaser", "Vibrato"};
static const char *const FLANGE_MODES[] = {"Classic", "Thru-Zero"};
static const char *const CHORDS[] = {"Unison", "Octaves", "Fifths", "Major", "Minor", "Sus4", "Maj7", "Min7"};
static const int CHORD_ST[8][4] = {{0, 0, 0, 0}, {0, 12, 24, -12}, {0, 7, 12, 19}, {0, 4, 7, 12}, {0, 3, 7, 12},
                                   {0, 5, 7, 12}, {0, 4, 7, 11}, {0, 3, 7, 10}};
static const char *const WAVES[] = {"Saw", "Square", "Sub", "Saw+Sub"};
static const char *const PATTERNS[] = {"Up", "Down", "Up-Down", "Random"};
static const char *const ARP_SETS[] = {"1-3-5", "1-3-5-8", "1-5-8", "1-4-5", "1-2-3-5", "1-5-8-10"};
static const int ARP_DEG[6][5] = {{3, 0, 2, 4}, {4, 0, 2, 4, 7}, {3, 0, 4, 7}, {3, 0, 3, 4}, {4, 0, 1, 2, 4}, {4, 0, 4, 7, 9}};
static const char *const SPRINGS[] = {"1", "2", "3"};
static const char *const PREPOST[] = {"Pre", "Post"};
static const char *const LATENCY[] = {"Low", "Normal"};
static const char *const VOICE_N[] = {"Single", "Dual"};
static const char *const SHIM_PITCHES[] = {"-12 st", "-5 st", "+7 st", "+12 st", "+19 st", "+24 st"};
static const float SHIM_ST[] = {-12, -5, 7, 12, 19, 24};
static const char *const CHORUS_MODES[] = {"Chorus", "Vibrato"};
/* DrivPdls: ten classic drive pedals and fifteen boutique ones, our own models of their kinds of circuit (near-miss
 * names, personal use). New ones go at the end (a saved Pedal is its place in this list). */
static const char *const PEDALS[] = {"Tube Scrmr", "Klone", "Rodent", "Big Muf", "Fuz Face", "Blues Drvr", "DS-l",
                                     "Timmie", "0CD", "Guvnr", "Kng o Tone", "Mornin Glo", "Jan Rae", "Zendrv", "Swt Honey",
                                     "BeeBee Pre", "EP Boostr", "0DR-l", "Sun Set", "C0T5O", "Hny Bee", "Plumz", "Bonzai",
                                     "Plexi Drv", "Brothrs"};
#define N_PEDALS ((int)(sizeof PEDALS / sizeof *PEDALS))
#define P_BONZAI 22
/* Bonzai's nine Tube Screamer voices (Knob 7, TS Voice; only Bonzai uses it) */
static const char *const TS_VOICES[] = {"0D-l", "8O8", "TS-9", "MSL", "TS-lO", "Exar", "Keely Mod", "Strong Mod", "TS-7"};
enum { CL_SOFT, CL_HARD, CL_ASYM, CL_MUFF, CL_MOSFET, CL_FACE };   /* clipping */
enum { TN_LP, TN_SCOOP, TN_TILT };                                  /* tone circuit */
typedef struct {
    float pre_hp;        /* input coupling (Hz): how much bass gets in */
    float mid_hp;        /* > 0: only the part above it is driven, the lows pass clean (the TS mid hump) */
    float gain_lo, gain_hi;   /* drive range (dB) */
    float bw_lo, bw_hi;  /* the gain stage's bandwidth (Hz), at no drive and at full (op-amps darken as they gain) */
    int clip, tone;
    float tone_lo, tone_hi;   /* TN_LP: the lowpass sweep (Hz) */
    float clean;         /* clean signal kept inside the pedal (the Klon's blend), at no drive */
    float trim;          /* output trim (dB): the pedals about as loud as each other */
} PedalDef;
static const PedalDef PEDAL_DEFS[] = {
    {30, 720, 0, 40, 9000, 6000, CL_SOFT, TN_LP, 700, 5000, 0, -1},        /* green screamer: mids, smooth */
    {80, 300, 6, 42, 10000, 8000, CL_ASYM, TN_TILT, 0, 0, 0.6f, -2.7f},    /* the transparent-ish gold one: clean blend */
    {60, 0, 12, 64, 20000, 2500, CL_HARD, TN_LP, 600, 16000, 0, -2.2f},      /* rodent: op-amp into hard diodes */
    {100, 0, 24, 56, 6000, 5000, CL_MUFF, TN_SCOOP, 0, 0, 0, 1.4f},         /* big fuzz: two stages, scooped mids */
    {20, 0, 16, 48, 9000, 7000, CL_FACE, TN_LP, 2500, 8000, 0, -1.1f},       /* round fuzz: biased, sputtering */
    {40, 0, 0, 44, 12000, 9000, CL_ASYM, TN_TILT, 0, 0, 0, -1.6f},           /* blue driver: wide, amp-like */
    {100, 0, 14, 54, 16000, 6000, CL_HARD, TN_SCOOP, 0, 0, 0, 1.3f},         /* orange distortion: hard, scooped */
    {40, 0, 0, 34, 16000, 12000, CL_SOFT, TN_LP, 2000, 14000, 0.15f, -2.9f},  /* the transparent one */
    {50, 0, 6, 50, 10000, 7000, CL_MOSFET, TN_LP, 1500, 9000, 0, -2.9f},     /* the obsessive one: MOSFET clipping */
    {60, 450, 10, 50, 12000, 7000, CL_HARD, TN_TILT, 0, 0, 0, -2.3f},         /* amp-in-a-box British crunch */
    /* the boutique ones */
    {50, 0, 0, 36, 12000, 9000, CL_SOFT, TN_LP, 1500, 9000, 0, -2.7f},         /* the king: bluesbreaker-style, open */
    {40, 0, 0, 36, 14000, 10000, CL_SOFT, TN_TILT, 0, 0, 0, -2.6f},            /* morning: brighter bluesbreaker */
    {40, 250, 0, 32, 16000, 12000, CL_SOFT, TN_TILT, 0, 0, 0.1f, -2.3f},       /* the sparkly transparent one */
    {60, 400, 6, 46, 9000, 5000, CL_SOFT, TN_LP, 800, 6000, 0, -3.2f},          /* zen: smooth, vocal mids (D-style amp) */
    {50, 0, 4, 40, 10000, 7000, CL_ASYM, TN_LP, 1200, 8000, 0, -1.6f},          /* honey: amp-like, a little uneven */
    {40, 0, 6, 44, 12000, 8000, CL_ASYM, TN_TILT, 0, 0, 0, -1.7f},              /* the preamp: thick, more gain */
    {20, 0, 0, 22, 20000, 18000, CL_SOFT, TN_TILT, 0, 0, 0, -1.3f},             /* the echo-preamp booster: clean, a bit hot */
    {40, 500, 0, 40, 9000, 6000, CL_SOFT, TN_TILT, 0, 0, 0, -2.2f},             /* green box: flatter mid hump */
    {40, 0, 0, 44, 14000, 9000, CL_ASYM, TN_TILT, 0, 0, 0, -1.6f},              /* sunset: wide, amp-like */
    {60, 0, 10, 50, 10000, 6000, CL_HARD, TN_LP, 1500, 10000, 0, -1.8f},        /* the hot plexi-style one */
    {50, 350, 6, 44, 10000, 7000, CL_SOFT, TN_LP, 1000, 8000, 0, -3.2f},        /* the bee: British overdrive */
    {30, 700, 0, 40, 11000, 7000, CL_SOFT, TN_LP, 900, 7000, 0, -1.3f},         /* green-style, louder and opener */
    {30, 720, 0, 40, 9000, 6000, CL_SOFT, TN_LP, 700, 5000, 0, -1.2f},          /* Bonzai: (its voice: TS_VOICE_DEFS) */
    {80, 0, 12, 52, 10000, 6000, CL_HARD, TN_TILT, 0, 0, 0, -1.8f},             /* plexi in a box */
    {40, 0, 6, 50, 12000, 7000, CL_ASYM, TN_TILT, 0, 0, 0.1f, -1.9f},           /* the brothers: boost into drive */
};
_Static_assert(sizeof PEDAL_DEFS / sizeof *PEDAL_DEFS == sizeof PEDALS / sizeof *PEDALS, "a PedalDef per pedal");
static const PedalDef TS_VOICE_DEFS[] = {   /* Bonzai's voices */
    {30, 600, 4, 42, 10000, 6000, CL_ASYM, TN_LP, 3500, 3500, 0, -1.7f},       /* the first: asymmetric, no tone sweep */
    {30, 720, 0, 40, 9000, 6000, CL_SOFT, TN_LP, 700, 5000, 0, -1.2f},          /* the 808 */
    {30, 720, 0, 40, 10000, 7000, CL_SOFT, TN_LP, 900, 7000, 0, -1.2f},         /* the 9: brighter */
    {40, 600, 10, 52, 9000, 5000, CL_SOFT, TN_LP, 800, 6000, 0, -3.4f},         /* the metal one */
    {30, 720, 0, 38, 9000, 6000, CL_SOFT, TN_LP, 700, 5500, 0, -0.8f},          /* the 10 */
    {30, 700, 6, 46, 9000, 6000, CL_SOFT, TN_LP, 800, 6000, 0, -3.0f},          /* the Exar chip: hotter */
    {25, 450, 2, 44, 11000, 7000, CL_SOFT, TN_LP, 900, 7000, 0, -3.0f},         /* the mod: fuller lows, more gain */
    {25, 300, 6, 50, 11000, 7000, CL_SOFT, TN_LP, 900, 7500, 0, -3.4f},         /* the strong mod */
    {30, 720, 4, 46, 9000, 6000, CL_HARD, TN_LP, 800, 6000, 0, -2.0f},          /* the 7 */
};

/* ==== the algorithms ========================================================================================= */
enum {   /* in the pop-up's order */
    /* delay */ A_BAND, A_BBD, A_DIGITAL, A_DUCKED, A_FPONG, A_HEADS, A_MODDLY, A_MULTITAP, A_REVDLY, A_TAPE, A_TAPCLOUD,
    A_VINTAGE,
    /* distortion */ A_FUZZOCT, A_CRUSH, A_FUZZDLY, A_BANDDRIVE, A_MIDDRIVE, A_PEDALS,
    /* pitch */ A_REVSHIFT, A_DIATONIC, A_VSHIFTER, A_PMODHARM, A_ARP, A_MICRO, A_OCTAVER, A_POG, A_BEND, A_POLYBEND, A_POLYSHIFT,
    A_SYNTH, A_STEPSHIFT, A_QUAD, A_RESONATOR, A_VOCAL, A_CORRECT,
    /* modulation */ A_CHORUS, A_FLANGER, A_TREMOLO, A_PHASER, A_VIBE, A_SFLANGER, A_SPHASER, A_TRICHORUS,
    /* reverb */ A_HALL, A_ROOM, A_PLATE, A_SPRING, A_SHIMMER, A_ABYSS, A_CLASSIC, A_CRUSHVERB, A_TREMVERB, A_WARP,
    /* granular */ A_GRAINWEB, A_GLITCH, A_GRAINMOD, A_STUTTER,
    A_OFF, kNumAlgos
};
enum { G_DELAY, G_DIST, G_PITCH, G_MOD, G_REVERB, G_GRAIN, G_NONE };
static const char *const GROUP_NAMES[] = {"Delay", "Distortion", "Pitch", "Modulation", "Reverb", "Granular", ""};

typedef struct { const char *name; int group; float mix; const char *about; Knob k[NK]; } Algo;
static const Algo ALGOS[kNumAlgos] = {
    [A_BAND] = {"Band Delay", G_DELAY, 40, "Repeats through a swept resonant band, can be pitched",
        {MS("Time", 20, 2000, 400), PCT("Feedback", 50), HZ("Band", 150, 6000, 900), PCT("Resonance", 50),
         PCT("Sweep", 40), HZ("Rate", 0.05f, 8, 0.4f), ST("Pitch", 0), PCT("Tone", 50)}},
    [A_BBD] = {"Bouqet Delay", G_DELAY, 40, "Bucket-brigade: dark, chorused, saturating repeats",
        {MS("Time", 20, 600, 300), PCT("Feedback", 45), PCT("Mod", 30), HZ("Rate", 0.05f, 8, 0.6f), HZ("Tone", 1200, 7000, 3000),
         PCT("Age", 30), NONE, NONE}},
    [A_DIGITAL] = {"Digital Delay", G_DELAY, 35, "Clean, exact repeats",
        {MS("Time", 10, 2000, 375), PCT("Feedback", 35), HZ("Low Cut", 20, 2000, 20), HZ("High Cut", 1000, 20000, 20000),
         PCT("Mod", 0), HZ("Rate", 0.05f, 8, 0.5f), MS("Spread", 0.5f, 100, 0.5f), CHO("Ping-Pong", ONOFF, 0)}},
    [A_DUCKED] = {"Ducked Delay", G_DELAY, 45, "Repeats duck while you play, swell up in the gaps",
        {MS("Time", 20, 2000, 450), PCT("Feedback", 45), LIN("Threshold", -60, 0, -30, "dB"), LIN("Duck", 0, 40, 18, "dB"),
         MS("Release", 20, 2000, 300), PCT("Tone", 50), PCT("Mod", 10), MS("Spread", 0.5f, 100, 10)}},
    [A_FPONG] = {"FiltrPong", G_DELAY, 45, "Repeats bounce L-R through sweeping resonant filters",
        {MS("Time", 20, 2000, 350), PCT("Feedback", 55), HZ("Filter", 150, 8000, 1200), PCT("Resonance", 55),
         PCT("Sweep", 60), HZ("Rate", 0.05f, 8, 0.3f), CHO("Type", FTYPES, 1), PCT("Width", 100)}},
    [A_HEADS] = {"HeadSpce", G_DELAY, 40, "Tape echo with three playback heads",
        {MS("Spacing", 40, 600, 180), CHO("Heads", HEADS, 6), PCT("Feedback", 45), PCT("Wow", 25), PCT("Flutter", 20),
         PCT("Saturate", 35), HZ("Tone", 1000, 10000, 3500), PCT("Spread", 40)}},
    [A_MODDLY] = {"Mod Delay", G_DELAY, 40, "Delay with a modulated, chorusing line",
        {MS("Time", 20, 2000, 400), PCT("Feedback", 40), PCT("Depth", 40), HZ("Rate", 0.05f, 8, 0.8f), CHO("Shape", SHAPES, 0),
         PCT("Tone", 50), MS("Spread", 0.5f, 100, 12), NONE}},
    [A_MULTITAP] = {"MultiTp", G_DELAY, 40, "Up to 10 taps: even, speeding up or slowing down",
        {MS("Span", 50, 2500, 800), INT("Taps", 2, 10, 6, ""), LIN("Spacing", -100, 100, 0, ""), LIN("Taper", -100, 100, -40, ""),
         PCT("Feedback", 20), PCT("Tone", 50), PCT("Width", 70), PCT("Diffuse", 0)}},
    [A_REVDLY] = {"Reverse", G_DELAY, 45, "The input played backwards, then repeated",
        {MS("Time", 150, 2000, 600), PCT("Feedback", 35), PCT("Tone", 50), PCT("Mod", 10), HZ("Rate", 0.05f, 8, 0.5f), NONE, NONE, NONE}},
    [A_TAPE] = {"Tape Echo", G_DELAY, 40, "Saturating tape loop with wow, flutter and hiss",
        {MS("Time", 40, 1500, 350), PCT("Feedback", 45), PCT("Wow", 30), PCT("Flutter", 25), PCT("Saturate", 40),
         HZ("Tone", 1000, 10000, 4000), PCT("Age", 25), PCT("Spread", 30)}},
    [A_TAPCLOUD] = {"UltraTp", G_DELAY, 45, "Up to 32 taps: rhythms, smears, reverbs, textures",
        {MS("Length", 20, 2000, 500), INT("Taps", 1, 32, 12, ""), MS("Predelay", 1, 1000, 1), LIN("Spread", -100, 100, 0, ""),
         LIN("Taper", -100, 100, 0, ""), PCT("Smear", 30), HZ("Chop", 0.1f, 20, 0.1f), PCT("Tone", 50)}},
    [A_VINTAGE] = {"Vintage Delay", G_DELAY, 40, "Early digital delay: fewer bits, lower sample rate",
        {MS("Time", 20, 2000, 420), PCT("Feedback", 40), INT("Bits", 6, 16, 12, ""), INT("Rate Div", 1, 8, 2, "x"),
         PCT("Tone", 45), PCT("Mod", 10), HZ("Mod Rate", 0.05f, 8, 0.6f), MS("Spread", 0.5f, 100, 0.5f)}},

    [A_FUZZOCT] = {"Aggravat3", G_DIST, 100, "Fuzz with an octave up, tremolo and envelope filter",
        {PCT("Drive", 60), PCT("Octave", 50), PCT("Tone", 55), LIN("Level", -24, 6, -6, "dB"), PCT("Trem", 0),
         HZ("Trem Rate", 0.5f, 15, 5), PCT("Envelope", 0), PCT("Env Q", 50)}},
    [A_CRUSH] = {"CrushStatn", G_DIST, 100, "Drive into compression, with power-supply sag",
        {PCT("Drive", 55), PCT("Sag", 30), PCT("Comp", 40), PCT("Tone", 55), LIN("Bass", -12, 12, 0, "dB"),
         LIN("Level", -24, 6, -6, "dB"), LIN("Gate", -90, -30, -90, "dB"), NONE}},
    [A_FUZZDLY] = {"PtchFuzz", G_DIST, 100, "Fuzz on two pitch-shifted voices, with a delay",
        {PCT("Fuzz", 60), PCT("Tone", 50), LIN("Level", -24, 6, -6, "dB"), ST("Pitch A", -12), ST("Pitch B", 12),
         MS("Delay", 1, 1000, 1), PCT("Feedback", 0), PCT("Pitch Mix", 50)}},
    [A_BANDDRIVE] = {"Sculpt3", G_DIST, 100, "Lows and highs driven separately, then blended",
        {HZ("Split", 100, 2000, 400), PCT("Low Drive", 30), PCT("High Drive", 60), LIN("Low Level", -24, 6, 0, "dB"),
         LIN("High Level", -24, 6, -3, "dB"), HZ("Filter", 1000, 16000, 7000), PCT("Comp", 30), NONE}},
    [A_PEDALS] = {"DrivPdls", G_DIST, 100, "25 drive and fuzz pedals, classic and boutique",
        {CHO("Pedal", PEDALS, 0), PCT("Drive", 50), PCT("Tone", 50), LIN("Level", -24, 6, -6, "dB"), LIN("Bass", -12, 12, 0, "dB"),
         PCT("Blend", 0), CHO("TS Voice", TS_VOICES, 1), NONE}},
    [A_MIDDRIVE] = {"WeedWackr", G_DIST, 100, "Mid-hump overdrive in the classic green-pedal style",
        {PCT("Drive", 45), PCT("Tone", 50), LIN("Level", -24, 12, -3, "dB"), HZ("Mid", 400, 1600, 720), LIN("Bass", -12, 6, -3, "dB"),
         CHO("Hard Clip", ONOFF, 0), PCT("Blend", 100), NONE}},

    [A_REVSHIFT] = {"Crystalz", G_PITCH, 45, "Reversed, pitch-shifted delays into a reverb",
        {ST("Pitch A", 12), ST("Pitch B", 7), MS("Delay A", 150, 2500, 500), MS("Delay B", 150, 2500, 750), PCT("Feedback", 45),
         PCT("Tone", 55), PCT("Mod", 10), PCT("Reverb", 30)}},
    [A_DIATONIC] = {"Diatonik", G_PITCH, 50, "Two voices that follow your notes in a key and scale",
        {CHO("Key", KEYS, 0), CHO("Scale", SCALES, 0), CHO("Voice 1", DEGREES, 9), CHO("Voice 2", DEGREES, 11),
         PCT("Level 1", 80), PCT("Level 2", 60), MS("Delay", 1, 500, 1), PCT("Width", 60)}},
    [A_VSHIFTER] = {"H91O", G_PITCH, 50, "Early rack harmonizer: raw splices, delay, feedback",
        {LIN("Pitch", -24, 24, 7, "st"), MS("Delay", 1, 1000, 1), PCT("Feedback", 0), CHO("Deglitch", ONOFF, 0), PCT("Tone", 50),
         INT("Stereo", 0, 30, 0, "c"), NONE, NONE}},
    [A_PMODHARM] = {"HarModulatr", G_PITCH, 50, "Two shifted voices with pitch vibrato and delay",
        {ST("Pitch A", -12), ST("Pitch B", 12), INT("Depth", 0, 100, 20, "c"), HZ("Rate", 0.05f, 8, 0.8f), MS("Delay", 1, 1000, 80),
         PCT("Feedback", 20), PCT("Tone", 50), PCT("Width", 70)}},
    [A_ARP] = {"HarPeggiatr", G_PITCH, 60, "Steps a shifted voice through a pattern in your key",
        {CHO("Key", KEYS, 0), CHO("Scale", SCALES, 0), CHO("Notes", ARP_SETS, 1), CHO("Pattern", PATTERNS, 0),
         HZ("Rate", 0.5f, 16, 4), MS("Glide", 1, 200, 5), INT("Octaves", 1, 2, 1, ""), PCT("Feedback", 0)}},
    [A_MICRO] = {"MicroPtch", G_PITCH, 50, "Slightly detuned, delayed copies for width",
        {INT("Detune", 0, 50, 9, "c"), MS("Delay L", 1, 100, 7), MS("Delay R", 1, 100, 11), PCT("Feedback", 0), PCT("Tone", 50),
         PCT("Drift", 20), NONE, NONE}},
    [A_OCTAVER] = {"Octaver", G_PITCH, 100, "Octaves below and above your playing",
        {PCT("Sub 1", 70), PCT("Sub 2", 0), PCT("Up", 0), PCT("Dry", 100), PCT("Tone", 50), PCT("Drive", 0), NONE, NONE}},
    [A_POG] = {"P0G3", G_PITCH, 100, "Polyphonic octaves: 2 down, 2 up, detune, filter, swell",
        {PCT("Sub", 0), PCT("Down", 60), PCT("Dry", 100), PCT("Up", 50), PCT("Up 2", 0), INT("Detune", 0, 50, 0, "c"),
         HZ("Filter", 200, 16000, 16000), LIN("Attack", 0, 2000, 0, "ms")}},
    [A_BEND] = {"PitchFlx", G_PITCH, 100, "Bend between two intervals (a Q-Link makes a pedal)",
        {PCT("Bend", 0), ST("Heel", 0), ST("Toe", 12), MS("Glide", 1, 500, 30), CHO("Voices", VOICE_N, 0), PCT("Tone", 50), NONE, NONE}},
    [A_POLYBEND] = {"PolyFlx", G_PITCH, 100, "Two voices bending in opposite directions",
        {PCT("Bend", 0), ST("Range A", 12), ST("Range B", -12), MS("Glide", 1, 500, 30), PCT("Level A", 100), PCT("Level B", 70),
         PCT("Tone", 50), NONE}},
    [A_POLYSHIFT] = {"Polyphny", G_PITCH, 50, "Two independent pitch voices for chords",
        {ST("Pitch A", -12), INT("Fine A", -50, 50, 0, "c"), ST("Pitch B", 7), INT("Fine B", -50, 50, 0, "c"),
         PCT("Level A", 100), PCT("Level B", 70), PCT("Width", 50), CHO("Latency", LATENCY, 1)}},
    [A_SYNTH] = {"PolySynt", G_PITCH, 70, "A synth voice that follows the note you play",
        {CHO("Wave", WAVES, 0), INT("Octave", -2, 1, -1, ""), HZ("Cutoff", 100, 8000, 900), PCT("Resonance", 40),
         PCT("Env", 60), MS("Attack", 1, 500, 5), MS("Release", 20, 2000, 300), MS("Glide", 1, 300, 20)}},
    [A_STEPSHIFT] = {"PrismShft", G_PITCH, 60, "A pitch sequence of four steps, with delay",
        {ST("Step 1", 0), ST("Step 2", 7), ST("Step 3", 12), ST("Step 4", 5), HZ("Rate", 0.5f, 16, 3), MS("Glide", 1, 200, 5),
         MS("Delay", 1, 1000, 1), PCT("Feedback", 20)}},
    [A_QUAD] = {"Quadravx", G_PITCH, 50, "Four voices in key (Unison on voice 4: off)",
        {CHO("Key", KEYS, 0), CHO("Scale", SCALES, 0), CHO("Voice 1", DEGREES, 9), CHO("Voice 2", DEGREES, 11),
         CHO("Voice 3", DEGREES, 14), CHO("Voice 4", DEGREES, 7), PCT("Width", 70), MS("Delay", 1, 300, 1)}},
    [A_RESONATOR] = {"Resonator", G_PITCH, 50, "Four tuned resonators ringing on a chord",
        {CHO("Root", KEYS, 9), CHO("Chord", CHORDS, 3), INT("Octave", 1, 5, 3, ""), PCT("Decay", 70), PCT("Tone", 60),
         PCT("Width", 60), PCT("Drive", 0), NONE}},
    [A_VOCAL] = {"VocalShft", G_PITCH, 100, "Shifts a voice and pulls it towards the scale",
        {ST("Pitch", 0), INT("Fine", -50, 50, 0, "c"), CHO("Key", KEYS, 0), CHO("Scale", SCALES, 0), PCT("Correct", 0),
         MS("Speed", 1, 300, 50), PCT("Tone", 50), NONE}},
    [A_CORRECT] = {"VocalTun", G_PITCH, 100, "Tunes a voice or instrument to the key and scale",
        {CHO("Key", KEYS, 0), CHO("Scale", SCALES, 0), MS("Speed", 1, 300, 20), PCT("Amount", 100), INT("Detune", -50, 50, 0, "c"),
         NONE, NONE, NONE}},

    [A_CHORUS] = {"Chorus", G_MOD, 50, "Three voices of modulated delay",
        {PCT("Depth", 45), HZ("Rate", 0.05f, 8, 0.6f), MS("Delay", 4, 30, 9), INT("Voices", 1, 3, 3, ""), PCT("Feedback", 0),
         PCT("Tone", 55), PCT("Width", 80), CHO("Shape", SHAPES, 0)}},
    [A_FLANGER] = {"Flanger", G_MOD, 50, "Short swept delay with resonance",
        {MS("Delay", 0.3f, 10, 1.5f), PCT("Depth", 60), HZ("Rate", 0.02f, 8, 0.25f), LIN("Feedback", -95, 95, 60, "%"),
         PCT("Tone", 60), PCT("Width", 60), CHO("Shape", SHAPES, 1), NONE}},
    [A_TREMOLO] = {"Tremolo", G_MOD, 100, "Level modulation, smooth to choppy, mono to panning",
        {PCT("Depth", 70), HZ("Rate", 0.5f, 20, 5), PCT("Shape", 20), PCT("Width", 0), NONE, NONE, NONE, NONE}},
    [A_PHASER] = {"Phaser", G_MOD, 50, "Swept allpass stages with feedback",
        {CHO("Stages", STAGES, 1), HZ("Center", 150, 4000, 700), PCT("Depth", 70), HZ("Rate", 0.02f, 8, 0.4f), PCT("Feedback", 40),
         PCT("Width", 50), CHO("Shape", SHAPES, 0), NONE}},
    [A_VIBE] = {"Even-Vib3", G_MOD, 50, "Photocell phase-shifter wobble, chorus or vibrato",
        {PCT("Intensity", 70), HZ("Speed", 0.3f, 10, 2), CHO("Mode", VIBE_MODES, 0), PCT("Throb", 40), PCT("Tone", 50),
         PCT("Width", 30), NONE, NONE}},
    [A_SFLANGER] = {"InstFlangr", G_MOD, 50, "Tape-style flanging that can pass through zero",
        {PCT("Manual", 30), PCT("Depth", 60), HZ("Rate", 0.02f, 4, 0.15f), LIN("Feedback", -95, 95, 0, "%"),
         CHO("Mode", FLANGE_MODES, 1), PCT("Tone", 60), PCT("Width", 40), NONE}},
    [A_SPHASER] = {"InstPhasr", G_MOD, 50, "Smooth multi-stage studio phasing",
        {PCT("Manual", 40), PCT("Depth", 70), HZ("Rate", 0.02f, 4, 0.2f), PCT("Feedback", 30), CHO("Stages", STAGES, 3),
         CHO("Mode", PHASE_MODES, 0), PCT("Width", 50), NONE}},

    [A_TRICHORUS] = {"TricraChrs", G_MOD, 50, "Three BBD voices, ensemble wobble and shimmer",
        {HZ("Rate", 0.05f, 8, 0.6f), PCT("Depth", 50), PCT("Ensemble", 35), PCT("Width", 80), HZ("Tone", 2000, 16000, 6300),
         PCT("Shimmer", 0), CHO("Shim Pitch", SHIM_PITCHES, 3), CHO("Mode", CHORUS_MODES, 0)}},

    [A_HALL] = {"Hall", G_REVERB, 35, "Large, smooth concert hall",
        {PCT("Size", 65), EXPK("Decay", 0.5f, 20, 3, "s"), MS("Predelay", 1, 1000, 20), PCT("Tone", 55), LIN("Low", -12, 6, 0, "dB"),
         PCT("Mod", 30), PCT("Early", 30), NONE}},
    [A_ROOM] = {"Room", G_REVERB, 30, "A small to medium room",
        {PCT("Size", 40), EXPK("Decay", 0.2f, 5, 0.8f, "s"), MS("Predelay", 1, 100, 5), PCT("Tone", 50), PCT("Early", 50),
         PCT("Mod", 15), NONE, NONE}},
    [A_PLATE] = {"Plate", G_REVERB, 30, "Bright, dense plate",
        {PCT("Size", 55), EXPK("Decay", 0.3f, 10, 2, "s"), MS("Predelay", 1, 200, 10), PCT("Tone", 70), PCT("Mod", 20),
         HZ("Low Cut", 20, 800, 120), NONE, NONE}},
    [A_SPRING] = {"Spring", G_REVERB, 30, "Amp-style spring tank, drip and all",
        {EXPK("Decay", 0.5f, 6, 2, "s"), PCT("Tension", 50), PCT("Tone", 50), CHO("Springs", SPRINGS, 1), PCT("Drip", 40),
         HZ("Low Cut", 20, 800, 150), NONE, NONE}},
    [A_SHIMMER] = {"ShimmrVerb", G_REVERB, 40, "Reverb whose tail climbs in pitch",
        {PCT("Size", 70), EXPK("Decay", 1, 20, 6, "s"), ST("Pitch A", 12), ST("Pitch B", 19), PCT("Shimmer", 50), PCT("Tone", 60),
         PCT("Mod", 30), NONE}},
    [A_ABYSS] = {"Blackhol3", G_REVERB, 50, "Enormous dark space; full Decay holds forever",
        {PCT("Size", 75), EXPK("Decay", 3, 100, 20, "s"), PCT("Swell", 0), PCT("Tone", 45), LIN("Low", -12, 6, 0, "dB"),
         PCT("Mod", 50), HZ("Rate", 0.05f, 4, 0.3f), MS("Predelay", 1, 2000, 1)}},
    [A_CLASSIC] = {"SP2O16", G_REVERB, 35, "80s studio reverb: early reflections, smooth tail",
        {PCT("Size", 55), EXPK("Decay", 0.3f, 15, 2.5f, "s"), MS("Predelay", 1, 1000, 30), PCT("Position", 50), PCT("Diffusion", 70),
         PCT("Tone", 55), LIN("Low", -12, 6, 0, "dB"), NONE}},
    [A_CRUSHVERB] = {"MangldVerb", G_REVERB, 40, "Reverb pushed into distortion",
        {PCT("Size", 60), EXPK("Decay", 0.5f, 15, 3, "s"), MS("Predelay", 1, 300, 20), PCT("Distortion", 50), CHO("Order", PREPOST, 1),
         PCT("Tone", 50), PCT("Mod", 30), NONE}},
    [A_TREMVERB] = {"TremoloVrb", G_REVERB, 40, "Reverb with a pulsing tremolo",
        {PCT("Size", 55), EXPK("Decay", 0.5f, 10, 2.5f, "s"), PCT("Tone", 55), PCT("Trem", 60), HZ("Trem Rate", 0.5f, 15, 4),
         PCT("Shape", 30), CHO("Order", PREPOST, 1), NONE}},
    [A_WARP] = {"Wormhol3", G_REVERB, 45, "A heavily bent, pitch-warping space",
        {PCT("Size", 70), EXPK("Decay", 1, 30, 8, "s"), PCT("Warp", 50), HZ("Rate", 0.05f, 4, 0.2f), ST("Pitch", 0), PCT("Tone", 50),
         PCT("Width", 80), NONE}},

    [A_GRAINWEB] = {"CosmicWb", G_GRAIN, 50, "Two pitched grain voices, reversing and feeding back",
        {MS("Grain", 20, 500, 120), HZ("Density", 1, 40, 12), ST("Pitch A", 12), ST("Pitch B", -12), MS("Delay", 10, 2000, 300),
         PCT("Feedback", 40), PCT("Reverse", 30), PCT("Tone", 55)}},
    [A_GLITCH] = {"Glitch", G_GRAIN, 60, "Stutters, clock jumps and bit-crushing at random",
        {HZ("Rate", 0.5f, 16, 4), PCT("Chance", 50), INT("Bits", 4, 16, 10, ""), INT("Alias", 1, 16, 1, "x"), MS("Length", 10, 500, 90),
         PCT("Reverse", 30), PCT("Jump", 40), PCT("Tone", 60)}},
    [A_GRAINMOD] = {"GrainMd", G_GRAIN, 50, "Grain delay moved by four slow LFOs",
        {MS("Grain", 20, 500, 150), HZ("Density", 1, 40, 10), MS("Delay", 10, 2000, 400), PCT("Feedback", 30), INT("Pitch Mod", 0, 1200, 100, "c"),
         PCT("Pos Mod", 40), PCT("Pan Mod", 60), HZ("LFO Rate", 0.02f, 4, 0.3f)}},
    [A_STUTTER] = {"Stutter", G_GRAIN, 100, "Catches a slice and repeats it, with tape-stop",
        {MS("Slice", 20, 1000, 125), INT("Repeats", 1, 16, 4, ""), HZ("Rate", 0.2f, 8, 1), PCT("Chance", 60), PCT("Fade", 20),
         PCT("Tape Stop", 0), PCT("Reverse", 0), NONE}},
    [A_OFF] = {"Off", G_NONE, 0, "Passes the sound through", {NONE, NONE, NONE, NONE, NONE, NONE, NONE, NONE}},
};

/* ==== parameters (the order is the VST index: saved projects and Q-Links refer to them by it) ============== */
enum { P_ALGO, P_OPEN, P_MIX, P_K, P_NAME = P_K + NK, P_INFO = P_NAME + NK, P_SLOT };   /* per slot */
enum { kRouting = 2 * P_SLOT, kOutput, kPresetBase, kProgBase = kPresetBase + 6, kBypass = kProgBase + 3, kBypassA, kBypassB,
       kLookA, kLookB, kNumParams };   /* (A/B Look: read-only, the page's colour for the slot: tools/pedal_looks.h) */
/* per slot: Preset, Preset <, Preset >; then Program, Program <, Program > (both slots and the routing); then
 * Bypass (both), A Bypass, B Bypass. A bypassed slot hears nothing but lets its tail ring out, the dry sound passes. */

/* MIDI (any channel), as on a pedal board: Program Change n loads program n+1; CC 80 / 81 / 82 bypass both / A / B
 * (64-127 bypassed, 0-63 on); CC 83 / 84 program down / up, CC 85 / 86 A preset down / up, CC 87 / 88 B preset
 * down / up (each on a value of 64 or more, after one below: a footswitch press). The bypasses are also plain
 * parameters, for MPC's own MIDI Learn and Q-Links. */
enum { CC_BYPASS = 80, CC_PROG_DOWN = 83, CC_PRESET_A_DOWN = 85, CC_PRESET_B_DOWN = 87, CC_LAST = 88 };

static int option_of(float v, int n) { return (int)lrintf((v < 0 ? 0 : v > 1 ? 1 : v) * (float)(n - 1)); }
static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float expmap(float lo, float hi, float t) { return lo * powf(hi / lo, t); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int finite_bits(float v) { uint32_t u; memcpy(&u, &v, 4); return (u & 0x7f800000u) != 0x7f800000u; }
static float db(float d) { return powf(10, d / 20); }

static float knob_value(const Knob *k, float n) {   /* 0..1 -> the knob's own units (a choice: its index) */
    n = clampf(n, 0, 1);
    switch (k->map) {
    case M_EXP: return expmap(k->lo, k->hi, n);
    case M_INT: case M_CHOICE: return (float)lrintf(lerp(k->lo, k->hi, n));
    default: return lerp(k->lo, k->hi, n);
    }
}
static float knob_norm(const Knob *k, float v) {   /* the reverse */
    if (!k->name || k->hi == k->lo) return 0;
    if (k->map == M_EXP) return clampf(logf(v / k->lo) / logf(k->hi / k->lo), 0, 1);
    return clampf((v - k->lo) / (k->hi - k->lo), 0, 1);
}

/* ==== building blocks ======================================================================================== */
typedef struct { float *b; int mask, w; } Line;   /* a delay line, a power of two long */
static int line_init(Line *l, int min_len) {
    int n = 1;
    while (n < min_len) n <<= 1;
    l->b = calloc((size_t)n, sizeof(float));
    l->mask = n - 1;
    l->w = 0;
    return l->b != NULL;
}
static void line_clear(Line *l) { if (l->b) memset(l->b, 0, (size_t)(l->mask + 1) * sizeof(float)); l->w = 0; }
static void line_push(Line *l, float x) { l->b[l->w] = x; l->w = (l->w + 1) & l->mask; }
static float line_tap(const Line *l, int d) { return l->b[(l->w - 1 - d) & l->mask]; }
static float line_read(const Line *l, float d) {   /* d samples ago (fractional; cubic) */
    if (d < 1) d = 1;
    if (d > (float)(l->mask - 3)) d = (float)(l->mask - 3);
    const int i = (int)d;
    const float f = d - (float)i;
    const float y0 = line_tap(l, i - 1), y1 = line_tap(l, i), y2 = line_tap(l, i + 1), y3 = line_tap(l, i + 2);
    const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

static float SINE[1025];
static void sine_init(void) { for (int i = 0; i <= 1024; i++) SINE[i] = sinf(2 * PI * (float)i / 1024); }
static float sine01(float ph) {   /* sin(2 pi ph) */
    const float x = (ph - floorf(ph)) * 1024;
    const int i = (int)x;
    return SINE[i] + (SINE[i + 1] - SINE[i]) * (x - (float)i);
}
static float tri01(float ph) { ph -= floorf(ph); return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph; }
static float hann01(float ph) { const float v = sine01(ph * 0.5f); return v * v; }
static float softclip(float x) { return x / (1 + fabsf(x)); }
static float tanh_fast(float x) { x = clampf(x, -3, 3); const float x2 = x * x; return x * (27 + x2) / (27 + 9 * x2); }

typedef struct { float z; } OnePole;
static float lp(OnePole *f, float x, float a) { f->z += a * (x - f->z); return f->z; }
static float hp(OnePole *f, float x, float a) { return x - lp(f, x, a); }
static float lp_coef(float hz, float fs) { const float a = 1 - expf(-2 * PI * hz / fs); return a > 1 ? 1 : a; }

/* state-variable filter (topology-preserving: stays stable while swept) */
typedef struct { float ic1, ic2; } Svf;
typedef struct { float g, k, a1, a2, a3; } SvfCo;
static SvfCo svf_co(float hz, float q, float fs) {
    SvfCo c;
    c.g = tanf(PI * clampf(hz, 10, fs * 0.45f) / fs);
    c.k = 1 / clampf(q, 0.5f, 25);
    c.a1 = 1 / (1 + c.g * (c.g + c.k));
    c.a2 = c.g * c.a1;
    c.a3 = c.g * c.a2;
    return c;
}
static void svf(Svf *f, const SvfCo *c, float x, float *lo, float *bp, float *hi) {
    const float v3 = x - f->ic2, v1 = c->a1 * f->ic1 + c->a2 * v3, v2 = f->ic2 + c->a2 * f->ic1 + c->a3 * v3;
    f->ic1 = 2 * v1 - f->ic1;
    f->ic2 = 2 * v2 - f->ic2;
    *lo = v2; *bp = v1; *hi = x - c->k * v1 - v2;
}

/* a pitch shifter: two grains (Hann windows, half a grain apart, so their gains add to one) reading a delay line
 * at `ratio` times the speed it's written. With `search`, each new grain starts where its sound best matches what
 * the other grain is playing (a correlation search), so the splices line up instead of cancelling the note;
 * without it (the "vintage" sound) the splices land anywhere. */
typedef struct { Line l; float d[2], ph[2]; int search, init; } Shifter;
static float grain_start(Shifter *s, float ratio, float win, int other, int search) {
    const float base = 8 + (float)s->search + (ratio > 1 ? (ratio - 1) * win : 0);
    if (!search) return base;
    float ref[32];   /* (32 points 6 samples apart, candidates 3 apart: light enough for eight shifters at once) */
    for (int i = 0; i < 32; i++) ref[i] = line_tap(&s->l, (int)(s->d[other] - (float)(i * 6) * ratio));
    float best = base, best_v = -1e30f;
    for (int o = -s->search; o <= s->search; o += 3) {
        const float c = base + (float)o;
        float xy = 0, yy = 1e-9f;
        for (int i = 0; i < 32; i++) { const float y = line_tap(&s->l, (int)(c - (float)(i * 6) * ratio)); xy += ref[i] * y; yy += y * y; }
        const float v = xy / sqrtf(yy);
        if (v > best_v) { best_v = v; best = c; }
    }
    return best;
}
static float shift_s(Shifter *s, float x, float ratio, float win, int search) {
    line_push(&s->l, x);
    ratio = clampf(ratio, 0.125f, 4.2f);
    if (!s->init) { s->ph[0] = 0; s->ph[1] = 0.5f; s->d[0] = s->d[1] = 8 + (float)s->search + (ratio > 1 ? (ratio - 1) * win : 0); s->init = 1; }
    float y = 0;
    for (int k = 0; k < 2; k++) {
        s->ph[k] += 1 / win;
        if (s->ph[k] >= 1) { s->ph[k] -= floorf(s->ph[k]); s->d[k] = grain_start(s, ratio, win, k ^ 1, search); }
        s->d[k] += 1 - ratio;
        y += line_read(&s->l, s->d[k]) * hann01(s->ph[k]);
    }
    return y;
}

typedef struct { Line l; int d; } Allpass;
static float allpass(Allpass *a, float x, float g) {
    const float v = line_tap(&a->l, a->d - 1), u = x + g * v;
    line_push(&a->l, u);
    return v - g * u;
}

/* a monophonic pitch tracker (YIN on a 4x decimated copy, every 512 samples) */
#define TRK_N 1024
typedef struct { float buf[TRK_N]; int w, dec, cnt; float acc, hz, conf; OnePole pre; } Tracker;
static void track(Tracker *t, float x, float fs) {
    t->acc += lp(&t->pre, x, 0.25f);
    if (++t->dec < 4) return;
    t->buf[t->w] = t->acc * 0.25f;
    t->w = (t->w + 1) & (TRK_N - 1);
    t->acc = 0; t->dec = 0;
    if (++t->cnt < 128) return;
    t->cnt = 0;
    enum { W = 320, TMAX = 300, TMIN = 6 };
    float d[TMAX + 1], x0[W + TMAX];
    for (int i = 0; i < W + TMAX; i++) x0[i] = t->buf[(t->w - (W + TMAX) + i) & (TRK_N - 1)];
    float energy = 0;
    for (int i = 0; i < W; i++) energy += x0[i] * x0[i];
    if (energy < 1e-5f) { t->conf = 0; return; }
    float run = 0;
    d[0] = 1;
    int best = -1;
    for (int tau = 1; tau <= TMAX; tau++) {
        float s = 0;
        for (int i = 0; i < W; i++) { const float e = x0[i] - x0[i + tau]; s += e * e; }
        run += s;
        d[tau] = s * (float)tau / (run + 1e-12f);
        if (tau > TMIN + 1 && best < 0 && d[tau - 1] < 0.15f && d[tau - 1] <= d[tau] && d[tau - 1] <= d[tau - 2]) best = tau - 1;
    }
    if (best < 0) { t->conf = 0; return; }
    const float a = d[best - 1], b = d[best], c = d[best + 1], den = a - 2 * b + c;
    const float tau = (float)best + (fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0);
    t->hz = fs / 4 / tau;
    t->conf = 1 - b;
}
static float midi_of(float hz) { return 69 + 12 * log2f(hz / 440); }
/* semitones from the scale note nearest at or below MIDI note m to the one `steps` scale degrees away */
static float diatonic_shift(float m, int key, int scale, int steps) {
    const int *st = SCALE_STEPS[scale];
    const int n = st[0];
    const int note = (int)lrintf(m), rel = ((note - key) % 12 + 12) % 12, oct = (note - key - rel) / 12;
    int idx = 0;
    for (int i = 0; i < n; i++) if (st[1 + i] <= rel) idx = i;
    int target = idx + steps, toct = oct;
    while (target < 0) { target += n; toct--; }
    while (target >= n) { target -= n; toct++; }
    return (float)(key + toct * 12 + st[1 + target] - note);
}
static float nearest_in_scale(float m, int key, int scale) {
    const int *st = SCALE_STEPS[scale];
    float best = m, bd = 1e9f;
    const int base = (int)floorf((m - (float)key) / 12);
    for (int o = -1; o <= 1; o++)
        for (int i = 0; i < st[0]; i++) {
            const float c = (float)(key + st[1 + i] + 12 * (base + o));
            if (fabsf(c - m) < bd) { bd = fabsf(c - m); best = c; }
        }
    return best;
}
/* a DEGREES choice (0..14) -> semitones to shift, for the note m in key/scale */
static float degree_shift(float m, int key, int scale, int choice) {
    const int steps = choice - 7;
    if (steps == 7 || steps == -7) return (float)(steps / 7 * 12);
    return diatonic_shift(m, key, scale, steps * SCALE_STEPS[scale][0] / 7);
}

/* ==== one slot =============================================================================================== */
#define NFDN 8
#define NSH 4
#define NGR 12
static const float FDN_MS[NFDN] = {29.7f, 37.1f, 41.1f, 43.7f, 47.9f, 53.3f, 59.1f, 67.3f};
typedef struct { float pos, inc, len, age, pan; int on; } Grain;

typedef struct {
    int algo;
    float n[NK], v[NK];             /* knobs: 0..1 and in their own units */
    float mix_n, mix_s;
    int tick;
    float fs;
    uint32_t rng;
    float lfo[4];
    Line dl[2], dl2[2];             /* 4.2 s and 2.1 s a side */
    Shifter sh[2][NSH];
    Svf svf[2][4];
    SvfCo sc[4];
    OnePole op[2][8];
    Allpass ap[2][12];
    Line fdn[NFDN];
    OnePole damp[NFDN], lowsh[NFDN];
    Tracker trk;
    Grain gr[2][NGR];
    float st[2][16];                /* per-side state, meaning per algorithm */
    float c[48];                    /* control values, meaning per algorithm (slot_control) */
    float env[2], gate;
} Slot;

static float rnd(Slot *s) { s->rng = s->rng * 1664525u + 1013904223u; return (float)(s->rng >> 8) / 16777216.0f; }   /* 0..1 */

static int slot_init(Slot *s, float fs) {
    int ok = 1;
    s->fs = fs;
    s->rng = 12345u;
    for (int c = 0; c < 2; c++) {
        ok &= line_init(&s->dl[c], (int)(fs * 4.2f));
        ok &= line_init(&s->dl2[c], (int)(fs * 2.1f));
        for (int k = 0; k < NSH; k++) { ok &= line_init(&s->sh[c][k].l, (int)(fs * 0.3f)); s->sh[c][k].search = (int)(fs * 0.012f); }
        for (int k = 0; k < 12; k++) ok &= line_init(&s->ap[c][k].l, (int)(fs * 0.12f));
    }
    for (int k = 0; k < NFDN; k++) ok &= line_init(&s->fdn[k], (int)(fs * FDN_MS[k] * 4.2f / 1000) + 8);
    return ok;
}
static void slot_free(Slot *s) {
    for (int c = 0; c < 2; c++) {
        free(s->dl[c].b); free(s->dl2[c].b);
        for (int k = 0; k < NSH; k++) free(s->sh[c][k].l.b);
        for (int k = 0; k < 12; k++) free(s->ap[c][k].l.b);
    }
    for (int k = 0; k < NFDN; k++) free(s->fdn[k].b);
}
static void slot_clear(Slot *s) {   /* back to silence (keeps the knobs) */
    for (int c = 0; c < 2; c++) {
        line_clear(&s->dl[c]); line_clear(&s->dl2[c]);
        for (int k = 0; k < NSH; k++) { line_clear(&s->sh[c][k].l); s->sh[c][k].init = 0; }
        for (int k = 0; k < 12; k++) { line_clear(&s->ap[c][k].l); s->ap[c][k].d = 2; }
        memset(s->svf[c], 0, sizeof s->svf[c]);
        memset(s->op[c], 0, sizeof s->op[c]);
        memset(s->gr[c], 0, sizeof s->gr[c]);
        memset(s->st[c], 0, sizeof s->st[c]);
    }
    for (int k = 0; k < NFDN; k++) { line_clear(&s->fdn[k]); s->damp[k].z = 0; s->lowsh[k].z = 0; }
    memset(&s->trk, 0, sizeof s->trk);
    memset(s->lfo, 0, sizeof s->lfo);
    memset(s->sc, 0, sizeof s->sc);
    s->env[0] = s->env[1] = s->gate = 0;
    s->tick = 0;
}

/* ---- the reverb network, shared by the reverbs ------------------------------------------------------------- */
typedef struct { float size, rt60, damp_hz, low_db, depth, rate; int ndiff; float diff_scale, diff_g; } VerbCfg;
static void verb_control(Slot *s, const VerbCfg *v) {   /* uses c[0..15] */
    const float fs = s->fs;
    for (int k = 0; k < NFDN; k++)
        s->c[k] = clampf(powf(10, -3 * FDN_MS[k] * v->size / 1000 / v->rt60), 0, 0.9999f) * 0.35355339f;   /* 1/sqrt(8): lossless mix */
    s->c[8] = lp_coef(v->damp_hz, fs);
    s->c[9] = v->size * fs / 1000;
    s->c[10] = v->depth * fs;
    s->c[11] = v->rate / fs;
    s->c[12] = (float)v->ndiff;
    static const float DIFF_MS[6] = {4.7f, 3.6f, 12.7f, 9.3f, 17.1f, 6.1f};
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < 6; k++) {
            const int d = (int)(DIFF_MS[k] * (c ? 1.13f : 1) * v->diff_scale * fs / 1000);
            s->ap[c][k].d = d < 2 ? 2 : d > s->ap[c][k].l.mask - 2 ? s->ap[c][k].l.mask - 2 : d;
        }
    s->c[13] = v->diff_g;
    s->c[18] = 0;   /* (no pitch shifting in the loop unless the algorithm sets it after this) */
    float gmax = 0;   /* extra low-frequency gain in the loop, kept below what would make it ring on */
    for (int k = 0; k < NFDN; k++) gmax = fmaxf(gmax, s->c[k] / 0.35355339f);
    s->c[14] = fminf(db(v->low_db), 0.995f / fmaxf(gmax, 1e-6f)) - 1;
    s->c[15] = lp_coef(250, fs);
}
static void verb_run(Slot *s, float inl, float inr, float *ol, float *or_, float extra_in) {
    float x[2] = {inl + extra_in, inr + extra_in};
    const int nd = (int)s->c[12];
    for (int c = 0; c < 2; c++) for (int k = 0; k < nd; k++) x[c] = allpass(&s->ap[c][k], x[c], s->c[13]);
    s->lfo[3] += s->c[11];
    s->lfo[3] -= floorf(s->lfo[3]);
    float v[NFDN];
    for (int k = 0; k < NFDN; k++) {
        float d = FDN_MS[k] * s->c[9];
        if (k & 1) d += s->c[10] * (1 + sine01(s->lfo[3] + 0.125f * (float)k));
        v[k] = line_read(&s->fdn[k], d);
    }
    *ol = (v[0] - v[2] + v[4] - v[6] + v[1] + v[5]) * 0.35f;
    *or_ = (v[1] - v[3] + v[5] - v[7] + v[2] + v[6]) * 0.35f;
    for (int h = 1; h < NFDN; h <<= 1)   /* Hadamard mix */
        for (int i = 0; i < NFDN; i += h << 1)
            for (int j = i; j < i + h; j++) { const float a = v[j], b = v[j + h]; v[j] = a + b; v[j + h] = a - b; }
    for (int k = 0; k < NFDN; k++) {
        float y = lp(&s->damp[k], v[k] * s->c[k], s->c[8]);
        y += lp(&s->lowsh[k], y, s->c[15]) * s->c[14];
        /* Shimmer / Warp: four of the lines are pitch-shifted on their way round (c[16], c[17]: the two ratios,
         * c[18]: how much). Inside the loop its gain stays under one, so it can't run away whatever the knobs */
        if (k < 4 && s->c[18] > 0.001f) y += (shift_s(&s->sh[k >> 1][k & 1], y, s->c[16 + (k & 1)], 0.06f * s->fs, 1) - y) * s->c[18];
        line_push(&s->fdn[k], clampf(y, -20, 20) + ((k & 1) ? x[1] : x[0]) * 0.5f);
    }
}

/* ---- per-algorithm control values (every 16 samples) -------------------------------------------------------- */
#define V(i) (s->v[i])
#define PC(i) (s->v[i] * 0.01f)
static float tone_cut(float pct) { return expmap(800, 18000, pct * 0.01f); }   /* a "Tone" percentage as a lowpass */

static void slot_control(Slot *s) {
    const float fs = s->fs;
    for (int i = 0; i < NK; i++) if (ALGOS[s->algo].k[i].name) s->v[i] = knob_value(&ALGOS[s->algo].k[i], s->n[i]);
    float *c = s->c;
    VerbCfg vc;
    switch (s->algo) {
    case A_BAND: c[20] = lp_coef(tone_cut(V(7)), fs); c[21] = powf(2, V(6) / 12); break;
    case A_BBD: c[20] = lp_coef(V(4), fs); c[21] = lp_coef(V(4) * 0.8f, fs); break;
    case A_DIGITAL: c[20] = lp_coef(V(2), fs); c[21] = lp_coef(V(3), fs); break;
    case A_DUCKED: c[20] = db(V(2)); c[21] = db(-V(3)); c[22] = expf(-1 / (V(4) * 0.001f * fs)); c[23] = lp_coef(tone_cut(V(5)), fs); break;
    case A_HEADS: case A_TAPE: c[20] = lp_coef(V(s->algo == A_TAPE ? 5 : 6), fs); c[21] = lp_coef(80, fs); break;
    case A_MODDLY: case A_MULTITAP: case A_REVDLY: case A_TAPCLOUD:
        c[20] = lp_coef(tone_cut(V(s->algo == A_TAPCLOUD ? 7 : s->algo == A_REVDLY ? 2 : 5)), fs);
        break;
    case A_VINTAGE: c[20] = lp_coef(lerp(2500, 12000, PC(4)), fs); c[21] = powf(2, V(2) - 1); break;
    case A_FUZZOCT: c[20] = lp_coef(lerp(1200, 9000, PC(2)), fs); c[21] = db(V(3)); break;
    case A_CRUSH: c[20] = lp_coef(lerp(1500, 12000, PC(3)), fs); c[21] = db(V(5)); c[22] = db(V(4)) - 1; c[23] = lp_coef(200, fs);
        c[24] = db(V(6)); break;
    case A_FUZZDLY: c[20] = lp_coef(lerp(1500, 9000, PC(1)), fs); c[21] = db(V(2)); c[22] = powf(2, V(3) / 12); c[23] = powf(2, V(4) / 12); break;
    case A_BANDDRIVE: c[20] = lp_coef(V(0), fs); c[21] = db(V(3)); c[22] = db(V(4)); c[23] = lp_coef(V(5), fs); break;
    case A_PEDALS: {
        const PedalDef *pd = (int)V(0) == P_BONZAI ? &TS_VOICE_DEFS[(int)V(6)] : &PEDAL_DEFS[(int)V(0)];
        c[20] = lp_coef(pd->pre_hp, fs);
        c[21] = pd->mid_hp > 0 ? lp_coef(pd->mid_hp, fs) : 0;
        c[22] = db(lerp(pd->gain_lo, pd->gain_hi, PC(1)));
        c[23] = lp_coef(lerp(pd->bw_lo, pd->bw_hi, PC(1)), 2 * fs);   /* (the gain stage runs at 2x) */
        c[24] = pd->tone == TN_LP ? lp_coef(expmap(pd->tone_lo, pd->tone_hi, PC(2)), fs) : pd->tone == TN_SCOOP ? lp_coef(650, fs) : lp_coef(1000, fs);
        c[25] = pd->tone == TN_SCOOP ? lp_coef(1100, fs) : db(lerp(-12, 9, PC(2)));
        c[26] = db(V(3) + pd->trim);
        c[27] = db(V(4)) - 1; c[28] = lp_coef(180, fs);
        c[29] = pd->clean * (1 - PC(1));
        c[30] = lp_coef(11000, 2 * fs);   /* the 2x stream's anti-alias filter (two poles) */
        c[31] = lp_coef(12, fs);          /* DC: asymmetric clipping leaves some */
        c[32] = (float)pd->clip; c[33] = (float)pd->tone;
        break;
    }
    case A_MIDDRIVE: c[21] = lp_coef(lerp(1500, 8000, PC(1)), fs); c[22] = db(V(2)); c[23] = db(V(4));
        c[24] = lp_coef(V(3) * 0.4f, fs); break;
    case A_REVSHIFT:   /* (and a plain reverb after the delays) */
        vc = (VerbCfg){1.6f, 3, 9000, 0, 0.0015f, 0.4f, 4, 1.2f, 0.65f};
        verb_control(s, &vc); c[20] = lp_coef(tone_cut(V(5)), fs); c[21] = powf(2, V(0) / 12); c[22] = powf(2, V(1) / 12); break;
    case A_VSHIFTER: c[20] = lp_coef(tone_cut(V(4)), fs); c[21] = powf(2, V(0) / 12); c[22] = powf(2, V(5) / 1200); break;
    case A_PMODHARM: c[20] = lp_coef(tone_cut(V(6)), fs); c[21] = powf(2, V(0) / 12); c[22] = powf(2, V(1) / 12); break;
    case A_MICRO: c[20] = lp_coef(tone_cut(V(4)), fs); c[21] = powf(2, V(0) / 1200); break;
    case A_OCTAVER: c[20] = lp_coef(tone_cut(V(4)), fs); break;
    case A_POG:
        s->sc[0] = svf_co(V(6), 1.2f, fs);
        c[20] = powf(2, V(5) / 1200);                                  /* detune: + on the left, - on the right */
        c[21] = V(7) < 1 ? 1 : 1 - expf(-1 / (V(7) * 0.001f * fs));   /* the swell's rise */
        c[22] = lp_coef(300, fs); c[23] = lp_coef(4, fs);             /* onset: a fast and a slow envelope */
        break;
    case A_BEND: case A_POLYBEND: c[20] = lp_coef(tone_cut(V(s->algo == A_BEND ? 5 : 6)), fs); c[21] = expf(-1 / (V(3) * 0.001f * fs)); break;
    case A_POLYSHIFT: c[21] = powf(2, (V(0) + V(1) * 0.01f) / 12); c[22] = powf(2, (V(2) + V(3) * 0.01f) / 12); break;
    case A_SYNTH: c[21] = expf(-1 / (V(5) * 0.001f * fs)); c[22] = expf(-1 / (V(6) * 0.001f * fs)); c[23] = expf(-1 / (V(7) * 0.001f * fs));
        c[24] = powf(2, V(1)); break;
    case A_STEPSHIFT: case A_ARP: c[21] = expf(-1 / (V(5) * 0.001f * fs)); break;
    case A_RESONATOR: {
        const int root = (int)V(0), ch = (int)V(1);
        for (int k = 0; k < 4; k++) {
            const float hz = 440 * powf(2, ((float)(root + 12 * ((int)V(2) + 1) + CHORD_ST[ch][k]) - 69) / 12);
            c[24 + k] = clampf(fs / hz, 8, (float)s->ap[0][1].l.mask - 8);
        }
        c[20] = lerp(0.8f, 0.995f, PC(3)); c[21] = lp_coef(tone_cut(V(4)), fs);
        break;
    }
    case A_VOCAL: c[20] = lp_coef(tone_cut(V(6)), fs); c[21] = expf(-1 / (V(5) * 0.001f * fs)); break;
    case A_CORRECT: c[20] = lp_coef(16000, fs); c[21] = expf(-1 / (V(2) * 0.001f * fs)); break;
    case A_CHORUS: c[20] = lp_coef(tone_cut(V(5)), fs); break;
    case A_FLANGER: c[20] = lp_coef(tone_cut(V(4)), fs); break;
    case A_SFLANGER: c[20] = lp_coef(tone_cut(V(5)), fs); break;
    case A_VIBE: c[20] = lp_coef(tone_cut(V(4)), fs); break;
    case A_TRICHORUS: c[20] = lp_coef(V(4), fs); c[21] = powf(2, SHIM_ST[(int)V(6)] / 12); c[22] = lp_coef(14000, fs);
        c[23] = 10.7f * fs / 1000; c[24] = lp_coef(9000, fs); break;
    case A_TREMOLO: c[21] = 1 + PC(2) * 15; c[22] = tanh_fast(c[21]); break;
    case A_HALL:
        vc = (VerbCfg){lerp(0.8f, 2.4f, PC(0)), V(1), lerp(2000, 16000, PC(3)), V(4), lerp(0, 0.003f, PC(5)), 0.4f, 4, 1.2f, 0.65f};
        verb_control(s, &vc); c[20] = V(2) * 0.001f * fs; c[21] = PC(6); break;
    case A_ROOM:
        vc = (VerbCfg){lerp(0.3f, 1.0f, PC(0)), V(1), lerp(2000, 14000, PC(3)), 0, lerp(0, 0.0015f, PC(5)), 0.6f, 2, 0.7f, 0.6f};
        verb_control(s, &vc); c[20] = V(2) * 0.001f * fs; c[21] = PC(4); break;
    case A_PLATE:
        vc = (VerbCfg){lerp(0.5f, 1.5f, PC(0)), V(1), lerp(3000, 19000, PC(3)), 0, lerp(0, 0.002f, PC(4)), 0.9f, 6, 0.9f, 0.72f};
        verb_control(s, &vc); c[20] = V(2) * 0.001f * fs; c[21] = lp_coef(V(5), fs); break;
    case A_SPRING: {
        const float tension = PC(1);
        for (int ch = 0; ch < 2; ch++)
            for (int k = 0; k < 12; k++) {   /* a chain of short allpasses: the spring's chirp */
                const int d = (int)((lerp(0.5f, 2.2f, tension) + 0.37f * (float)k + (ch ? 0.21f : 0)) * fs / 1000);
                s->ap[ch][k].d = d < 2 ? 2 : d;
            }
        const float loop = lerp(0.035f, 0.06f, tension);
        c[20] = lp_coef(lerp(1500, 6000, PC(2)), fs); c[21] = lp_coef(V(5), fs);
        c[22] = powf(10, -3 * loop / V(0));   /* loop gain for Decay */
        c[23] = loop * fs;
        break;
    }
    case A_SHIMMER:
        vc = (VerbCfg){lerp(0.8f, 2.6f, PC(0)), V(1), lerp(2500, 16000, PC(5)), 0, lerp(0, 0.003f, PC(6)), 0.3f, 4, 1.3f, 0.65f};
        /* (the two voices a few cents apart, as in the H90's own Shimmer presets: +1204 / +1193 cents) */
        verb_control(s, &vc); c[16] = powf(2, V(2) / 12 + 4.0f / 1200); c[17] = powf(2, V(3) / 12 - 7.0f / 1200); c[18] = PC(4) * 0.7f;
        break;
    case A_ABYSS: {
        const int frozen = V(1) >= 99.5f;
        vc = (VerbCfg){lerp(1.5f, 4.0f, PC(0)), frozen ? 1e6f : V(1), lerp(700, 12000, PC(3)), frozen ? 0 : V(4), lerp(0, 0.006f, PC(5)), V(6), 6, 2.5f, 0.7f};
        verb_control(s, &vc); c[20] = PC(2); c[21] = (float)frozen;
        /* a small space with a long Decay stores far more than the default one (Size 75%, 20 s) and built up past
         * the ceiling (the H90's Shoegazer: Size 0, ~23 s): its input is scaled down by that, never up */
        const float g = powf(10, -3 * 0.0474f * vc.size / vc.rt60), gref = powf(10, -3 * 0.0474f * 3.375f / 20);
        c[22] = frozen ? 1 : fminf(1, sqrtf((1 - g * g) / (1 - gref * gref)));
        break;
    }
    case A_CLASSIC:
        vc = (VerbCfg){lerp(0.7f, 2.2f, PC(0)), V(1), lerp(2000, 14000, PC(5)), V(6), 0.0008f, 0.5f, 4, 1.0f, lerp(0.3f, 0.75f, PC(4))};
        verb_control(s, &vc); c[20] = V(2) * 0.001f * fs; c[21] = PC(3); break;
    case A_CRUSHVERB:
        vc = (VerbCfg){lerp(0.8f, 2.2f, PC(0)), V(1), 9000, 0, lerp(0, 0.003f, PC(6)), 0.5f, 4, 1.2f, 0.65f};
        verb_control(s, &vc); c[20] = V(2) * 0.001f * fs; c[21] = lp_coef(tone_cut(V(5)), fs); break;
    case A_TREMVERB:
        vc = (VerbCfg){lerp(0.7f, 2.2f, PC(0)), V(1), lerp(2000, 15000, PC(2)), 0, 0.001f, 0.5f, 4, 1.1f, 0.65f};
        verb_control(s, &vc); c[21] = 1 + PC(5) * 15; c[22] = tanh_fast(c[21]); break;
    case A_WARP:
        vc = (VerbCfg){lerp(1.2f, 3.2f, PC(0)), V(1), 16000, 0, lerp(0.002f, 0.012f, PC(2)), V(3), 6, 2.0f, 0.7f};
        verb_control(s, &vc); c[24] = lp_coef(tone_cut(V(5)), fs);
        c[16] = powf(2, V(4) / 12); c[17] = powf(2, -V(4) / 24); c[18] = V(4) != 0 ? 0.35f : 0;
        break;
    case A_GRAINWEB: c[20] = lp_coef(tone_cut(V(7)), fs); break;
    case A_GRAINMOD: c[20] = lp_coef(tone_cut(60), fs); break;
    case A_GLITCH: c[20] = lp_coef(tone_cut(V(7)), fs); c[21] = powf(2, V(2) - 1); break;
    default: break;
    }
}

/* ---- the algorithms, one sample (in: l r, out: wet) --------------------------------------------------------- */
static float lfo_shape(float ph, int shape, Slot *s, int ch) {
    switch (shape) {
    case 1: return tri01(ph);
    case 2: return sine01(ph) >= 0 ? 1.0f : -1.0f;
    case 3: {   /* smoothed random: a new target each cycle (state in st[ch][13..15]) */
        float *val = &s->st[ch][15], *tg = &s->st[ch][14], *last = &s->st[ch][13];
        if (ph < *last) *tg = rnd(s) * 2 - 1;
        *last = ph;
        *val += 0.002f * (*tg - *val);
        return *val;
    }
    default: return sine01(ph);
    }
}
static float adv(float *ph, float hz, float fs) { *ph += hz / fs; *ph -= floorf(*ph); return *ph; }
/* a delay time, slewed so turning it bends the pitch like tape instead of clicking */
static float slew(float *cur, float target) { if (*cur <= 0) *cur = target; *cur += 0.0006f * (target - *cur); return *cur; }
static Line *res_line(Slot *s, int ch, int k) { return k == 0 ? &s->dl[ch] : k == 2 ? &s->dl2[ch] : &s->ap[ch][k].l; }

static void run_algo(Slot *s, const float in[2], float out[2]) {
    const float fs = s->fs, ms = fs * 0.001f;
    const int ctl = (s->tick & 15) == 1;   /* a sample on which the swept filters recompute */
    float *c = s->c;
    switch (s->algo) {
    case A_BAND: {
        const float ph = adv(&s->lfo[0], V(5), fs);
        for (int ch = 0; ch < 2; ch++) {
            if (ctl) s->sc[ch] = svf_co(V(2) * (1 + PC(4) * 0.8f * sine01(ph + 0.25f * (float)ch)), lerp(0.7f, 12, PC(3)), fs);
            float y = line_read(&s->dl[ch], slew(&s->st[ch][0], V(0) * ms)), lo, bp, hi;
            svf(&s->svf[ch][0], &s->sc[ch], y, &lo, &bp, &hi);
            y = lp(&s->op[ch][0], bp * s->sc[ch].k * 1.5f, c[20]);   /* (k = 1/Q: the band's peak at unity) */
            if (V(6) != 0) y = shift_s(&s->sh[ch][0], y, c[21], 0.05f * fs, 1);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(1) * 0.98f));
        }
        break;
    }
    case A_BBD: {
        const float ph = adv(&s->lfo[0], V(3), fs);
        for (int ch = 0; ch < 2; ch++) {
            const float d = slew(&s->st[ch][0], V(0) * ms) + PC(2) * 0.003f * fs * (1 + sine01(ph + 0.3f * (float)ch));
            float y = lp(&s->op[ch][1], lp(&s->op[ch][0], line_read(&s->dl[ch], d), c[20]), c[21]);
            y += (rnd(s) - 0.5f) * PC(5) * 0.004f;
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(1) * lerp(1.0f, 1.25f, PC(5))));
        }
        break;
    }
    case A_DIGITAL: {
        const float ph = adv(&s->lfo[0], V(5), fs);
        const int pp = V(7) > 0.5f;
        float y[2];
        for (int ch = 0; ch < 2; ch++) {
            const float d = slew(&s->st[ch][0], (V(0) + (ch ? V(6) : 0)) * ms) + PC(4) * 0.002f * fs * (1 + sine01(ph + 0.25f * (float)ch));
            y[ch] = lp(&s->op[ch][1], hp(&s->op[ch][0], line_read(&s->dl[ch], d), c[20]), c[21]);
            out[ch] = y[ch];
        }
        for (int ch = 0; ch < 2; ch++)   /* ping-pong: the mono input goes in on the left, repeats cross over */
            line_push(&s->dl[ch], (pp ? (ch ? 0 : (in[0] + in[1]) * 0.5f) : in[ch]) + tanh_fast((pp ? y[ch ^ 1] : y[ch]) * PC(1) * 0.98f));
        break;
    }
    case A_DUCKED: {
        const float lvl = fmaxf(fabsf(in[0]), fabsf(in[1]));
        s->env[0] = lvl > s->env[0] ? lvl : s->env[0] * c[22];
        s->gate += 0.002f * ((s->env[0] > c[20] ? c[21] : 1) - s->gate);
        const float ph = adv(&s->lfo[0], 0.5f, fs);
        for (int ch = 0; ch < 2; ch++) {
            const float d = slew(&s->st[ch][0], (V(0) + (ch ? V(7) : 0)) * ms) + PC(6) * 0.002f * fs * (1 + sine01(ph + 0.25f * (float)ch));
            const float y = lp(&s->op[ch][0], line_read(&s->dl[ch], d), c[23]);
            out[ch] = y * s->gate;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(1) * 0.98f));
        }
        break;
    }
    case A_FPONG: {
        const float ph = adv(&s->lfo[0], V(5), fs);
        const int type = (int)V(6);
        float y[2];
        for (int ch = 0; ch < 2; ch++) {
            if (ctl) s->sc[ch] = svf_co(V(2) * (1 + PC(4) * 0.85f * sine01(ph + 0.5f * (float)ch)), lerp(0.7f, 10, PC(3)), fs);
            float lo, bp, hi;
            svf(&s->svf[ch][0], &s->sc[ch], line_read(&s->dl[ch], slew(&s->st[ch][0], V(0) * ms)), &lo, &bp, &hi);
            const float k = s->sc[ch].k;   /* 1/Q: keeps the resonant peak near unity */
            y[ch] = (type == 0 ? lo : type == 1 ? bp : hi) * (type == 1 ? k * 1.4f : fminf(1, 1.6f * k));
        }
        const float mid = (y[0] + y[1]) * 0.5f;
        out[0] = lerp(mid, y[0], PC(7));
        out[1] = lerp(mid, y[1], PC(7));
        line_push(&s->dl[0], (in[0] + in[1]) * 0.5f + tanh_fast(y[1] * PC(1) * 0.98f));
        line_push(&s->dl[1], tanh_fast(y[0] * PC(1) * 0.98f));
        break;
    }
    case A_HEADS: case A_TAPE: {
        const int tape = s->algo == A_TAPE;
        const float wow = PC(tape ? 2 : 3), flut = PC(tape ? 3 : 4), sat = PC(tape ? 4 : 5), spread = PC(7);
        const float p1 = adv(&s->lfo[0], 0.6f, fs), p2 = adv(&s->lfo[1], 7.1f, fs);
        const float mod = wow * 0.004f * fs * (1 + sine01(p1)) + flut * 0.0004f * fs * sine01(p2);
        static const int MASK[7] = {1, 2, 4, 3, 5, 6, 7};   /* HEADS[] -> which of heads 1, 2, 3 play */
        const int m = tape ? 1 : MASK[(int)V(1)];
        for (int ch = 0; ch < 2; ch++) {
            const float base = slew(&s->st[ch][0], V(0) * ms) * (1 + spread * 0.04f * (float)ch);
            float y = 0;
            int n = 0;
            for (int h = 0; h < 3; h++) if (m & (1 << h)) { y += line_read(&s->dl[ch], base * (float)(h + 1) + mod); n++; }
            y = hp(&s->op[ch][1], lp(&s->op[ch][0], y, c[20]), c[21]);
            if (tape) y += (rnd(s) - 0.5f) * PC(6) * 0.003f;
            out[ch] = n > 1 ? y * 0.7f : y;
            const float drive = 1 + sat * 3;
            /* small-signal loop gain = Feedback x 1.05 (the heads' average goes back, not their sum): past ~95% it
             * runs away into the saturation, like a real one */
            line_push(&s->dl[ch], in[ch] + tanh_fast(y / (float)n * PC(tape ? 1 : 2) * 1.05f * drive) / drive);
        }
        break;
    }
    case A_MODDLY: {
        const float ph = adv(&s->lfo[0], V(3), fs);
        for (int ch = 0; ch < 2; ch++) {
            const float m = lfo_shape(ph + 0.25f * (float)ch, (int)V(4), s, ch);
            const float d = slew(&s->st[ch][0], (V(0) + (ch ? V(6) : 0)) * ms) + PC(2) * 0.006f * fs * (1 + m);
            const float y = lp(&s->op[ch][0], line_read(&s->dl[ch], d), c[20]);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(1) * 0.98f));
        }
        break;
    }
    case A_MULTITAP: {
        const int n = (int)V(1);
        const float span = slew(&s->st[0][0], V(0) * ms), curve = V(2) * 0.01f, taper = V(3) * 0.01f;
        float acc[2] = {0, 0};
        for (int t = 0; t < n; t++) {
            float u = (float)(t + 1) / (float)n;
            u = curve >= 0 ? powf(u, 1 + curve * 2) : 1 - powf(1 - u, 1 - curve * 2);
            const float g = clampf(1 + taper * (taper > 0 ? (u - 1) : -u), 0, 1) / sqrtf((float)n) * 1.4f;
            const float pan = (t & 1 ? 1 : -1) * PC(6) * 0.5f;
            for (int ch = 0; ch < 2; ch++) acc[ch] += line_read(&s->dl[ch], span * u) * g * (ch ? 0.5f + pan : 0.5f - pan) * 2;
        }
        for (int ch = 0; ch < 2; ch++) {
            float y = lp(&s->op[ch][0], acc[ch], c[20]);
            if (PC(7) > 0.01f)
                for (int k = 0; k < 4; k++) { s->ap[ch][k].d = (int)((3.1f + 4.3f * (float)k + (float)ch) * ms); y = allpass(&s->ap[ch][k], y, 0.6f * PC(7)); }
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(line_read(&s->dl[ch], span) * PC(4) * 0.98f));
        }
        break;
    }
    case A_REVDLY: case A_REVSHIFT: {   /* grains read backwards: the read head runs back while the write head runs on */
        const int rs = s->algo == A_REVSHIFT;
        for (int ch = 0; ch < 2; ch++) {
            const float ratio = rs ? (ch ? c[22] : c[21]) : 1;   /* (the read head goes back up to grain * (1 + ratio)) */
            const float grain = clampf((rs ? V(2 + ch) : V(0)) * ms, 0.05f * fs, fminf(2.5f * fs, ((float)s->dl[ch].mask - 0.03f * fs) / (1 + ratio)));
            float *ph = &s->st[ch][1];
            *ph += 1 / grain;
            *ph -= floorf(*ph);
            const float wob = PC(rs ? 6 : 3) * 0.002f * fs * (1 + sine01(adv(&s->lfo[ch], rs ? 0.3f : V(4), fs)));
            float y = 0;
            for (int k = 0; k < 2; k++) {
                float p = *ph + 0.5f * (float)k;
                p -= floorf(p);
                y += line_read(&s->dl[ch], 4 + p * grain * (1 + ratio) + wob) * hann01(p);
            }
            y = lp(&s->op[ch][0], y, c[20]);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(rs ? 4 : 1) * 0.9f));
        }
        if (rs && PC(7) > 0.001f) { float l, r; verb_run(s, out[0], out[1], &l, &r, 0); out[0] += l * PC(7); out[1] += r * PC(7); }
        break;
    }
    case A_TAPCLOUD: {
        const int n = (int)V(1);
        const float len = slew(&s->st[0][0], V(0) * ms), pre = V(2) * ms, spread = V(3) * 0.01f, taper = V(4) * 0.01f;
        const float chop = V(6) > 0.15f ? 0.5f + 0.5f * sine01(adv(&s->lfo[0], V(6), fs)) : 1;
        float acc[2] = {0, 0};
        for (int t = 0; t < n; t++) {
            float u = n > 1 ? (float)t / (float)(n - 1) : 0;
            u = spread >= 0 ? powf(u, 1 + spread * 2) : 1 - powf(1 - u, 1 - spread * 2);
            const float g = clampf(1 + taper * (taper > 0 ? (u - 1) : -u), 0, 1) / sqrtf((float)n) * 1.3f;
            for (int ch = 0; ch < 2; ch++) acc[ch] += line_read(&s->dl[ch], pre + len * u + (float)(ch * (t % 3)) * 0.4f * ms) * g;
        }
        const float sm = PC(5);
        for (int ch = 0; ch < 2; ch++) {
            float y = lp(&s->op[ch][0], acc[ch], c[20]);
            if (sm > 0.01f)
                for (int k = 0; k < 6; k++) {
                    s->ap[ch][k].d = (int)((5.3f + 7.9f * (float)k + 1.3f * (float)ch) * ms * (0.5f + sm));
                    y = allpass(&s->ap[ch][k], y, 0.7f * sm);
                }
            out[ch] = y * chop;
            line_push(&s->dl[ch], in[ch]);
        }
        break;
    }
    case A_VINTAGE: {
        const float ph = adv(&s->lfo[0], V(6), fs);
        const int div = (int)V(3);
        for (int ch = 0; ch < 2; ch++) {
            const float d = slew(&s->st[ch][0], (V(0) + (ch ? V(7) : 0)) * ms) + PC(5) * 0.002f * fs * (1 + sine01(ph + 0.25f * (float)ch));
            const float y0 = line_read(&s->dl[ch], d);
            if (++s->st[ch][2] >= (float)div) { s->st[ch][2] = 0; s->st[ch][3] = floorf(y0 * c[21] + 0.5f) / c[21]; }   /* hold + quantise */
            const float y = lp(&s->op[ch][1], lp(&s->op[ch][0], s->st[ch][3], c[20]), c[20]);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(1) * 0.98f));
        }
        break;
    }

    case A_FUZZOCT: {
        const float trem = 1 - PC(4) * 0.5f * (1 - sine01(adv(&s->lfo[0], V(5), fs)));
        for (int ch = 0; ch < 2; ch++) {
            const float x = in[ch] * (1 + PC(0) * 60);
            float y = softclip(x + PC(1) * (fabsf(x) - 0.3f));   /* rectifying adds the octave */
            y = hp(&s->op[ch][1], y, 0.004f);
            if (PC(6) > 0.01f) {   /* envelope filter */
                const float e = fabsf(in[ch]);
                s->env[ch] = e > s->env[ch] ? s->env[ch] + 0.01f * (e - s->env[ch]) : s->env[ch] * 0.9995f;
                if (ctl) s->sc[ch] = svf_co(300 + 6000 * clampf(s->env[ch] * 8 * PC(6), 0, 1), lerp(1, 8, PC(7)), fs);
                float lo, bp, hi;
                svf(&s->svf[ch][0], &s->sc[ch], y, &lo, &bp, &hi);
                y = bp * 1.5f;
            }
            out[ch] = lp(&s->op[ch][0], y, c[20]) * c[21] * trem;
        }
        break;
    }
    case A_CRUSH: {
        const float lvl = fmaxf(fabsf(in[0]), fabsf(in[1]));
        s->env[0] += (lvl > s->env[0] ? 0.01f : 0.0003f) * (lvl - s->env[0]);
        const float sag = 1 - PC(1) * clampf(s->env[0] * 4, 0, 0.8f);   /* loud playing pulls the supply down */
        const float comp = 1 / (1 + PC(2) * 8 * s->env[0]);
        s->gate += 0.001f * ((s->env[0] > c[24] ? 1.0f : 0.0f) - s->gate);
        for (int ch = 0; ch < 2; ch++) {
            const float x = (in[ch] + lp(&s->op[ch][2], in[ch], c[23]) * c[22]) * (1 + PC(0) * 40) * comp;
            float y = tanh_fast(x * sag + 0.1f * PC(1)) - tanh_fast(0.1f * PC(1));
            y = lp(&s->op[ch][0], y, c[20]);
            out[ch] = hp(&s->op[ch][1], y, 0.003f) * c[21] * s->gate;
        }
        break;
    }
    case A_FUZZDLY: {   /* each voice (you, Pitch A, Pitch B) fuzzed on its own, so the intervals stay clear */
        const float g = 1 + PC(0) * 80;
        for (int ch = 0; ch < 2; ch++) {
            const float x = in[ch];
            const float a = shift_s(&s->sh[ch][0], x, c[22] * (ch ? 1.0012f : 1), 0.03f * fs, 1);
            const float b = shift_s(&s->sh[ch][1], x, c[23] * (ch ? 1 : 1.0012f), 0.03f * fs, 1);
            const float voices = (softclip(a * g) + softclip(b * g)) * 0.5f;
            const float d = line_read(&s->dl[ch], slew(&s->st[ch][0], V(5) * ms));
            line_push(&s->dl[ch], voices + tanh_fast(d * PC(6) * 0.95f));
            const float y = lerp(softclip(x * g), d, PC(7));
            out[ch] = tanh_fast(lp(&s->op[ch][0], hp(&s->op[ch][1], y, 0.004f), c[20]) * c[21] / 3) * 3;   /* (under the ceiling) */
        }
        break;
    }
    case A_BANDDRIVE: {
        const float lvl = fmaxf(fabsf(in[0]), fabsf(in[1]));
        s->env[0] += (lvl > s->env[0] ? 0.01f : 0.0003f) * (lvl - s->env[0]);
        const float comp = 1 / (1 + PC(6) * 6 * s->env[0]);
        for (int ch = 0; ch < 2; ch++) {
            const float lo = lp(&s->op[ch][0], in[ch], c[20]), hi = in[ch] - lo;
            const float yl = tanh_fast(lo * (1 + PC(1) * 30)) * c[21];
            const float yh = softclip(hi * (1 + PC(2) * 60)) * c[22];
            out[ch] = lp(&s->op[ch][1], (yl + yh) * comp, c[23]);
        }
        break;
    }
    case A_PEDALS: {
        const int clip = (int)c[32], tone = (int)c[33];
        for (int ch = 0; ch < 2; ch++) {
            float *st = s->st[ch];
            OnePole *op = s->op[ch];
            const float x = hp(&op[0], in[ch], c[20]);
            float lows = 0, drv = x;
            if (c[21] > 0) { lows = lp(&op[1], x, c[21]); drv = x - lows; }   /* the mid hump: lows pass clean */
            /* the gain stage and clipper at twice the rate (two half-steps), then filtered back down */
            float y = 0;
            for (int h = 0; h < 2; h++) {
                const float u = (h ? drv : 0.5f * (st[0] + drv)) * c[22];
                float v = lp(&op[2], u, c[23]);
                switch (clip) {
                case CL_SOFT: v = tanh_fast(v); break;
                case CL_HARD: v = clampf(v, -1, 1); v = v - v * v * v * 0.15f; break;
                case CL_ASYM: v = v > 0 ? tanh_fast(v) : tanh_fast(v * 1.5f) * 0.7f; break;
                case CL_MOSFET: {   /* open top, harder bottom */
                    const float b = clampf(v, -0.85f, 0);
                    v = v > 0 ? tanh_fast(v * 0.8f) * 1.15f : b * (1 - 0.1f * b * b);
                    break;
                }
                case CL_MUFF: {   /* two clipping stages with a coupling cap between them */
                    v = tanh_fast(v);
                    st[1] += 0.02f * (v - st[1]);
                    v = tanh_fast((v - st[1]) * 6);
                    break;
                }
                default: {   /* the round fuzz: biased, so one side clips first and it gates when soft */
                    const float b = 0.35f;
                    v = tanh_fast(v + b) - tanh_fast(b);
                    v = v > 0 ? v : v * 1.3f;
                    v = clampf(v, -0.9f, 0.9f);
                    break;
                }
                }
                y = lp(&op[4], lp(&op[3], v, c[30]), c[30]);
            }
            st[0] = drv;
            y += lows * 0.9f + x * c[29];
            st[2] += c[31] * (y - st[2]);   /* DC out */
            y -= st[2];
            if (tone == TN_LP) y = lp(&op[5], y, c[24]);
            else if (tone == TN_SCOOP) { const float lo = lp(&op[5], y, c[24]), hi = y - lp(&op[6], y, c[25]); y = lerp(lo, hi, PC(2)) * 1.5f; }
            else { const float lo = lp(&op[5], y, c[24]); y = lo + (y - lo) * c[25]; }
            y += lp(&op[7], y, c[28]) * c[27];   /* Bass */
            out[ch] = lerp(y * c[26], in[ch], PC(5) * 0.999f);   /* Blend: some of your clean signal */
        }
        break;
    }
    case A_MIDDRIVE: {
        for (int ch = 0; ch < 2; ch++) {
            const float low = lp(&s->op[ch][3], in[ch], c[24]);
            const float x = (in[ch] - low) + low * c[23];   /* less bass into the clipper: the mid hump */
            float y = x * (1 + PC(0) * 50);
            y = V(5) > 0.5f ? clampf(y, -0.7f, 0.7f) : tanh_fast(y);
            y = lp(&s->op[ch][1], y, c[21]);
            out[ch] = lerp(in[ch], y * c[22], PC(6));
        }
        break;
    }

    case A_DIATONIC: case A_QUAD: case A_ARP: case A_VOCAL: case A_CORRECT: case A_SYNTH: {
        track(&s->trk, (in[0] + in[1]) * 0.5f, fs);
        if (s->trk.conf > 0.5f && s->trk.hz > 40 && s->trk.hz < 2000) s->st[0][4] = midi_of(s->trk.hz);
        const float m = s->st[0][4] > 0 ? s->st[0][4] : 60;
        const float win = 0.04f * fs;
        if (s->algo == A_SYNTH) {
            const float e = fmaxf(fabsf(in[0]), fabsf(in[1]));
            s->env[0] = e > s->env[0] ? e + c[21] * (s->env[0] - e) : e + c[22] * (s->env[0] - e);
            const float target = 440 * powf(2, (m - 69) / 12) * c[24];
            float *f = &s->st[0][6], *ph = &s->st[0][7], *ph2 = &s->st[0][8];
            *f = target + c[23] * (*f - target);
            adv(ph, *f, fs);
            adv(ph2, *f * 0.5f, fs);
            const int w = (int)V(0);
            const float saw = 2 * *ph - 1, sq = *ph < 0.5f ? 1.0f : -1.0f, sub = *ph2 < 0.5f ? 1.0f : -1.0f;
            const float osc = w == 0 ? saw : w == 1 ? sq : w == 2 ? sub : 0.6f * saw + 0.6f * sub;
            if (ctl) s->sc[0] = svf_co(V(2) * (1 + PC(4) * 6 * clampf(s->env[0] * 4, 0, 1)), lerp(0.7f, 8, PC(3)), fs);
            float lo, bp, hi;
            svf(&s->svf[0][0], &s->sc[0], osc, &lo, &bp, &hi);
            out[0] = out[1] = lo * clampf(s->env[0] * 3, 0, 1) * 0.5f;
            break;
        }
        const int key = (int)V(s->algo == A_VOCAL ? 2 : 0), scale = (int)V(s->algo == A_VOCAL ? 3 : 1);
        if (s->algo == A_DIATONIC || s->algo == A_QUAD) {
            const int quad = s->algo == A_QUAD, nv = quad ? 4 : 2;
            const float dly = V(quad ? 7 : 6) * ms + 2, width = PC(quad ? 6 : 7);
            for (int ch = 0; ch < 2; ch++) line_push(&s->dl[ch], in[ch]);
            float acc[2] = {0, 0};
            for (int k = 0; k < nv; k++) {
                const int choice = (int)V(2 + k);
                if (quad && k == 3 && choice == 7) continue;   /* Unison on voice 4: off */
                float *cur = &s->st[1][5 + k];
                *cur += 0.01f * (degree_shift(m, key, scale, choice) - *cur);
                const float ratio = powf(2, *cur / 12), lvl = quad ? 0.6f : PC(4 + k), pan = width * (k & 1 ? 0.5f : -0.5f);
                for (int ch = 0; ch < 2; ch++)
                    acc[ch] += shift_s(&s->sh[ch][k], line_read(&s->dl[ch], dly), ratio, win, 1) * lvl * (ch ? 1 + pan : 1 - pan);
            }
            out[0] = acc[0]; out[1] = acc[1];
            break;
        }
        if (s->algo == A_ARP) {
            float *ph = &s->lfo[0];
            const float before = *ph;
            adv(ph, V(4), fs);
            const int set = (int)V(2), per = ARP_DEG[set][0], cnt = per * (int)V(6), pat = (int)V(3);
            float *stepf = &s->st[1][0], *dir = &s->st[1][1];
            int step = (int)*stepf;
            if (*ph < before) {   /* the next note of the pattern */
                if (pat == 0) step = (step + 1) % cnt;
                else if (pat == 1) step = (step + cnt - 1) % cnt;
                else if (pat == 2) {
                    if (*dir == 0) *dir = 1;
                    if (step + (int)*dir < 0 || step + (int)*dir >= cnt) *dir = -*dir;
                    step = (int)clampf((float)(step + (int)*dir), 0, (float)(cnt - 1));
                } else step = (int)(rnd(s) * (float)cnt) % cnt;
            }
            step %= cnt;
            *stepf = (float)step;
            const int deg = ARP_DEG[set][1 + step % per];   /* in seven-note terms (7 = the octave) */
            const float st = diatonic_shift(m, key, scale, deg * SCALE_STEPS[scale][0] / 7) + 12 * (float)(step / per);
            float *cur = &s->st[0][9];
            *cur = st + c[21] * (*cur - st);
            for (int ch = 0; ch < 2; ch++) {
                const float y = shift_s(&s->sh[ch][0], in[ch] + s->st[ch][10] * PC(7) * 0.9f, powf(2, *cur / 12), win, 1);
                s->st[ch][10] = y;
                out[ch] = y;
            }
            break;
        }
        /* Vocal Shift / Pitch Correct: towards the scale note (by Correct / Amount), at Speed */
        const float target = nearest_in_scale(m, key, scale);
        const float amt = PC(s->algo == A_VOCAL ? 4 : 3);
        const float want = (target - m) * amt + (s->algo == A_VOCAL ? V(0) + V(1) * 0.01f : V(4) * 0.01f);
        float *cur = &s->st[0][11];
        *cur = want + c[21] * (*cur - want);
        const float ratio = powf(2, *cur / 12);
        for (int ch = 0; ch < 2; ch++) out[ch] = lp(&s->op[ch][0], shift_s(&s->sh[ch][0], in[ch], ratio, 0.03f * fs, 1), c[20]);
        break;
    }
    case A_VSHIFTER: {
        for (int ch = 0; ch < 2; ch++) {
            const float r = c[21] * (ch ? c[22] : 1 / c[22]);
            const float y = lp(&s->op[ch][0], shift_s(&s->sh[ch][0], line_read(&s->dl[ch], V(1) * ms + 2), r, 0.045f * fs, V(3) > 0.5f), c[20]);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(2) * 0.95f));
        }
        break;
    }
    case A_PMODHARM: {
        const float m = sine01(adv(&s->lfo[0], V(3), fs)) * V(2) / 1200;
        const float wa = powf(2, m), wb = 1 / wa;
        for (int ch = 0; ch < 2; ch++) {
            const float x = line_read(&s->dl[ch], V(4) * ms + 2);
            const float a = shift_s(&s->sh[ch][0], x, c[21] * wa, 0.04f * fs, 1), b = shift_s(&s->sh[ch][1], x, c[22] * wb, 0.04f * fs, 1);
            const float v = lp(&s->op[ch][0], lerp((a + b) * 0.5f, ch ? b : a, PC(7)), c[20]);
            out[ch] = v;
            line_push(&s->dl[ch], in[ch] + tanh_fast(v * PC(5) * 0.9f));
        }
        break;
    }
    case A_MICRO: {
        const float drift = 1 + PC(5) * 0.003f * sine01(adv(&s->lfo[0], 0.3f, fs));
        for (int ch = 0; ch < 2; ch++) {
            const float x = line_read(&s->dl[ch], V(1 + ch) * ms + 2);
            const float y = lp(&s->op[ch][0], shift_s(&s->sh[ch][0], x, (ch ? 1 / c[21] : c[21]) * drift, 0.03f * fs, 1), c[20]);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(3) * 0.9f));
        }
        break;
    }
    case A_OCTAVER: {
        for (int ch = 0; ch < 2; ch++) {
            const float x = in[ch];
            const float s1 = shift_s(&s->sh[ch][0], x, 0.5f, 0.05f * fs, 1);
            const float s2 = PC(1) > 0.01f ? shift_s(&s->sh[ch][1], x, 0.25f, 0.07f * fs, 1) : 0;
            const float up = PC(2) > 0.01f ? shift_s(&s->sh[ch][2], x, 2.0f, 0.03f * fs, 1) : 0;
            float y = s1 * PC(0) + s2 * PC(1) + up * PC(2);
            if (PC(5) > 0.01f) y = tanh_fast(y * (1 + PC(5) * 10)) / (1 + PC(5) * 2);
            out[ch] = lp(&s->op[ch][0], y, c[20]) + x * PC(3);
        }
        break;
    }
    case A_POG: {   /* four grain shifters a side (their correlation search keeps chords together) */
        static const float RATIO[4] = {0.25f, 0.5f, 2, 4}, WIN[4] = {0.07f, 0.05f, 0.03f, 0.025f};
        const int lv[4] = {0, 1, 3, 4};
        for (int ch = 0; ch < 2; ch++) {
            const float x = in[ch], det = ch ? 1 / c[20] : c[20];
            float wet = 0;
            for (int k = 0; k < 4; k++)
                if (PC(lv[k]) > 0.005f) wet += shift_s(&s->sh[ch][k], x, RATIO[k] * det, WIN[k] * fs, 1) * PC(lv[k]);
            float lo, bp, hi;
            svf(&s->svf[ch][0], &s->sc[0], wet, &lo, &bp, &hi);
            float *st = s->st[ch];   /* the swell: each new note's octaves rise over Attack */
            const float a = fabsf(x);
            st[0] += c[22] * (a - st[0]);
            st[1] += c[23] * (a - st[1]);
            if (st[0] > 2 * st[1] + 0.003f && st[2] > 0.5f) st[2] = 0;
            st[2] += c[21] * (1 - st[2]);
            out[ch] = lo * st[2] + x * PC(2);
        }
        break;
    }
    case A_BEND: case A_POLYBEND: {
        const int poly = s->algo == A_POLYBEND;
        float *b = &s->st[0][0];
        *b = PC(0) + c[21] * (*b - PC(0));
        const float sa = poly ? lerp(0, V(1), *b) : lerp(V(1), V(2), *b);
        const float sb = poly ? lerp(0, V(2), *b) : sa - 12;
        const int dual = poly || V(4) > 0.5f;
        const float ra = powf(2, sa / 12), rb = powf(2, sb / 12);
        for (int ch = 0; ch < 2; ch++) {
            float y = shift_s(&s->sh[ch][0], in[ch], ra, 0.04f * fs, 1) * (poly ? PC(4) : 1);
            if (dual) y += shift_s(&s->sh[ch][1], in[ch], rb, 0.04f * fs, 1) * (poly ? PC(5) : 0.7f);
            out[ch] = lp(&s->op[ch][0], y, c[20]);
        }
        break;
    }
    case A_POLYSHIFT: {
        const float win = (V(7) < 0.5f ? 0.02f : 0.045f) * fs, w = PC(6) * 0.5f;
        for (int ch = 0; ch < 2; ch++) {
            const float a = shift_s(&s->sh[ch][0], in[ch], c[21], win, 1) * PC(4);
            const float b = shift_s(&s->sh[ch][1], in[ch], c[22], win, 1) * PC(5);
            out[ch] = ch ? a * (1 - w) + b * (1 + w) : a * (1 + w) + b * (1 - w);
        }
        break;
    }
    case A_STEPSHIFT: {
        float *ph = &s->lfo[0];
        const float before = *ph;
        adv(ph, V(4), fs);
        if (*ph < before) s->st[1][0] = (float)(((int)s->st[1][0] + 1) & 3);
        const float st = V((int)s->st[1][0]);
        float *cur = &s->st[0][9];
        *cur = st + c[21] * (*cur - st);
        const float ratio = powf(2, *cur / 12);
        for (int ch = 0; ch < 2; ch++) {
            const float y = shift_s(&s->sh[ch][0], line_read(&s->dl[ch], V(6) * ms + 2), ratio, 0.04f * fs, 1);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(7) * 0.9f));
        }
        break;
    }
    case A_RESONATOR: {   /* four tuned combs (one line each), damped by Tone */
        for (int ch = 0; ch < 2; ch++) {
            const float x = PC(6) > 0.01f ? tanh_fast(in[ch] * (1 + PC(6) * 8)) : in[ch];
            float y = 0;
            for (int k = 0; k < 4; k++) {
                Line *l = res_line(s, ch, k);
                const float own = line_read(l, c[24 + k] * (1 + (ch ? 0.003f : 0) * PC(5)));
                y += own;
                line_push(l, x * 0.3f + lp(&s->op[ch][1 + k], own, c[21]) * c[20]);
            }
            out[ch] = y * 0.35f;
        }
        break;
    }

    case A_CHORUS: {
        const float ph = adv(&s->lfo[0], V(1), fs);
        const int nv = (int)V(3);
        for (int ch = 0; ch < 2; ch++) {
            float y = 0;
            for (int k = 0; k < nv; k++)
                y += line_read(&s->dl[ch], V(2) * ms + PC(0) * 0.003f * fs * (1 + lfo_shape(ph + (float)k / 3 + 0.17f * (float)ch * PC(6), (int)V(7), s, ch)));
            y = lp(&s->op[ch][0], y * 1.2f / (float)nv, c[20]);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(4) * 0.7f));
        }
        break;
    }
    case A_FLANGER: case A_SFLANGER: {
        const int studio = s->algo == A_SFLANGER;
        const float ph = adv(&s->lfo[0], V(2), fs);
        const int tz = studio && V(4) > 0.5f;
        const float base = studio ? lerp(0.3f, 8, PC(0)) * ms : V(0) * ms, fb = V(3) * 0.01f;
        for (int ch = 0; ch < 2; ch++) {
            const float m = lfo_shape(ph + PC(studio ? 6 : 5) * 0.25f * (float)ch, studio ? 1 : (int)V(6), s, ch);
            float y = lp(&s->op[ch][0], line_read(&s->dl[ch], base * (1 + PC(1) * 0.95f * m) + 2), c[20]);
            /* thru-zero: the dry side is delayed by the sweep's middle, so the two cross */
            const float dry = tz ? line_read(&s->dl2[ch], base + 2) : in[ch];
            if (tz) line_push(&s->dl2[ch], in[ch]);
            out[ch] = (dry + (tz ? -y : y)) * 0.6f;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * fb * 0.95f));
        }
        break;
    }
    case A_TREMOLO: case A_TREMVERB: {
        const int tv = s->algo == A_TREMVERB, post = tv && V(6) > 0.5f;
        const float ph = adv(&s->lfo[0], V(tv ? 4 : 1), fs), depth = PC(tv ? 3 : 0);
        float x[2] = {in[0], in[1]};
        if (post) verb_run(s, x[0], x[1], &x[0], &x[1], 0);
        for (int ch = 0; ch < 2; ch++) {
            const float w = tanh_fast(sine01(ph + (tv ? 0 : PC(3) * 0.5f) * (float)ch) * c[21]) / c[22];
            x[ch] *= 1 - depth * 0.5f * (1 - w);
        }
        if (tv && !post) verb_run(s, x[0], x[1], &x[0], &x[1], 0);
        out[0] = x[0]; out[1] = x[1];
        break;
    }
    case A_PHASER: case A_SPHASER: {
        const int studio = s->algo == A_SPHASER;
        static const int NST[4] = {4, 6, 8, 12};
        const int n = NST[(int)V(studio ? 4 : 0)];
        const float ph = adv(&s->lfo[0], V(studio ? 2 : 3), fs);
        const float centre = studio ? expmap(150, 4000, PC(0)) : V(1), depth = PC(studio ? 1 : 2), fbk = PC(studio ? 3 : 4);
        const int vib = studio && V(5) > 0.5f;
        for (int ch = 0; ch < 2; ch++) {
            const float sw = lfo_shape(ph + PC(studio ? 6 : 5) * 0.25f * (float)ch, studio ? 0 : (int)V(6), s, ch);
            const float f = clampf(centre * expf(sw * depth * 2.5f * 0.693147f), 20, fs * 0.45f);
            const float w = PI * f / fs, a = (1 - w) / (1 + w);
            float y = in[ch] + s->st[ch][0] * fbk * 0.85f;
            for (int k = 0; k < n; k++) { const float o = -a * y + s->st[ch][1 + k]; s->st[ch][1 + k] = y + a * o; y = o; }
            s->st[ch][0] = y;
            out[ch] = vib ? y : (in[ch] + y) * 0.5f;
        }
        break;
    }
    case A_VIBE: {   /* a lamp (rises fast, falls slow) lighting four photocells; Throb makes the swing lopsided */
        static const float BASE[4] = {60, 300, 900, 2200};
        const float ph = adv(&s->lfo[0], V(1), fs);
        for (int ch = 0; ch < 2; ch++) {
            float l = 0.5f + 0.5f * sine01(ph + PC(5) * 0.25f * (float)ch);
            l = l * lerp(1, l, PC(3));
            float *lamp = &s->st[ch][8];
            *lamp += (l > *lamp ? 0.01f : 0.002f) * (l - *lamp);
            float y = in[ch];
            for (int k = 0; k < 4; k++) {
                const float w = PI * clampf(BASE[k] * (1 + PC(0) * 4 * *lamp), 20, fs * 0.45f) / fs, a = (1 - w) / (1 + w);
                const float o = -a * y + s->st[ch][1 + k];
                s->st[ch][1 + k] = y + a * o;
                y = o;
            }
            y = lp(&s->op[ch][0], y, c[20]);
            out[ch] = V(2) > 0.5f ? y : (in[ch] + y) * 0.5f;
        }
        break;
    }

    case A_TRICHORUS: {   /* as the Tri Chorus plugin: three BBD voices a third of a cycle apart, panned L/C/R */
        const float ph = adv(&s->lfo[0], V(0), fs), fast = adv(&s->lfo[1], V(0) * 9.5f, fs);
        const int vib = V(7) > 0.5f;
        const float w = PC(3), ens = PC(2);
        const float pan[3][2] = {{lerp(0.6f, 1.0f, w), lerp(0.6f, 0.0f, w)}, {0.7f, 0.7f}, {lerp(0.6f, 0.0f, w), lerp(0.6f, 1.0f, w)}};
        const float mono = lp(&s->op[0][7], (in[0] + in[1]) * 0.5f, c[22]);
        line_push(&s->dl[0], tanh_fast(mono * 1.2f) / 1.2f);
        const float swing = PC(1) * (vib ? 0.008f : 0.0045f) * fs;
        float wet[2] = {0, 0};
        for (int v = 0; v < 3; v++) {
            const float off = (float)v / 3, m = sine01(ph + off) + ens * 0.35f * sine01(fast + off * 1.7f);
            float y = line_read(&s->dl[0], c[23] + swing * 0.5f * (1 + m) + 2);
            y = lp(&s->op[1][v], lp(&s->op[0][v], y, c[20]), c[20]);   /* the BBD's darkening */
            wet[0] += y * pan[v][0];
            wet[1] += y * pan[v][1];
        }
        for (int ch = 0; ch < 2; ch++) {
            wet[ch] *= 0.75f;
            if (PC(5) > 0.001f) wet[ch] += lp(&s->op[ch][6], shift_s(&s->sh[ch][0], wet[ch], c[21] * (ch ? 1.003f : 1), 0.05f * fs, 1), c[24]) * PC(5) * 0.8f;
            out[ch] = wet[ch];
        }
        break;
    }
    case A_HALL: case A_ROOM: case A_PLATE: case A_CLASSIC: case A_CRUSHVERB: {
        const float pre = c[20];
        for (int ch = 0; ch < 2; ch++) line_push(&s->dl2[ch], in[ch]);
        float x[2] = {line_read(&s->dl2[0], pre + 1), line_read(&s->dl2[1], pre + 1)};
        if (s->algo == A_PLATE) { x[0] = hp(&s->op[0][0], x[0], c[21]); x[1] = hp(&s->op[1][0], x[1], c[21]); }
        const float drive = 1 + PC(3) * 30;
        if (s->algo == A_CRUSHVERB && V(4) < 0.5f) for (int ch = 0; ch < 2; ch++) x[ch] = tanh_fast(x[ch] * drive) * 0.5f;
        float l, r;
        verb_run(s, x[0], x[1], &l, &r, 0);
        if (s->algo == A_HALL || s->algo == A_ROOM || s->algo == A_CLASSIC) {   /* early reflections */
            static const float ER[6] = {7.3f, 11.9f, 17.7f, 23.1f, 31.3f, 41.9f};
            const float sz = s->algo == A_ROOM ? lerp(0.4f, 1.2f, PC(0)) : lerp(0.8f, 2.0f, PC(0));
            float el = 0, er = 0;
            for (int k = 0; k < 6; k++) {
                el += line_read(&s->dl2[0], pre + ER[k] * sz * ms) * (0.8f - 0.1f * (float)k);
                er += line_read(&s->dl2[1], pre + ER[k] * sz * 1.07f * ms) * (0.8f - 0.1f * (float)k);
            }
            const float early = s->algo == A_CLASSIC ? 1 - c[21] : c[21], late = s->algo == A_CLASSIC ? 0.5f + c[21] : 1;
            l = l * late + el * early * 0.4f;
            r = r * late + er * early * 0.4f;
        }
        if (s->algo == A_CRUSHVERB) {
            if (V(4) > 0.5f) { l = tanh_fast(l * drive) * 0.5f; r = tanh_fast(r * drive) * 0.5f; }
            l = lp(&s->op[0][1], l, c[21]); r = lp(&s->op[1][1], r, c[21]);
        }
        out[0] = l; out[1] = r;
        break;
    }
    case A_SPRING: {
        const int n = (int)V(3) + 1;
        for (int ch = 0; ch < 2; ch++) {
            float x = hp(&s->op[ch][0], in[ch], c[21]);
            x += x * PC(4) * 2 * fabsf(x);   /* drip: louder hits splash more */
            float y = x + s->st[ch][0];
            for (int k = 0; k < 12; k++) y = allpass(&s->ap[ch][k], y, 0.6f);
            line_push(&s->dl2[ch], y);
            float acc = 0;
            for (int k = 0; k < n; k++) acc += line_read(&s->dl2[ch], c[23] * (1 + 0.13f * (float)k));
            acc /= (float)n;
            s->st[ch][0] = clampf(lp(&s->op[ch][1], acc, c[20]) * c[22], -4, 4);
            out[ch] = acc;
        }
        break;
    }
    case A_SHIMMER: verb_run(s, in[0], in[1], &out[0], &out[1], 0); break;   /* (the shifting is inside the loop) */
    case A_ABYSS: {
        for (int ch = 0; ch < 2; ch++) line_push(&s->dl2[ch], in[ch]);
        float x[2] = {line_read(&s->dl2[0], V(7) * ms), line_read(&s->dl2[1], V(7) * ms)};
        if (c[20] > 0.01f) {   /* Swell: the reverb hears less of you while you play, so it blooms after */
            const float lvl = fmaxf(fabsf(in[0]), fabsf(in[1]));
            s->env[0] += (lvl > s->env[0] ? 0.005f : 0.0002f) * (lvl - s->env[0]);
            const float g = 1 / (1 + c[20] * 20 * s->env[0]);
            x[0] *= g; x[1] *= g;
        }
        if (c[21] > 0.5f) { x[0] *= 0.02f; x[1] *= 0.02f; }   /* frozen: it keeps what it has */
        x[0] *= c[22]; x[1] *= c[22];
        verb_run(s, x[0], x[1], &out[0], &out[1], 0);
        out[0] *= 0.7f; out[1] *= 0.7f;
        break;
    }
    case A_WARP: {
        float l, r;
        verb_run(s, in[0], in[1], &l, &r, 0);
        const float mid = (l + r) * 0.5f;
        out[0] = lp(&s->op[0][1], lerp(mid, l, PC(6)), c[24]);
        out[1] = lp(&s->op[1][1], lerp(mid, r, PC(6)), c[24]);
        break;
    }

    case A_GRAINWEB: case A_GRAINMOD: {
        const int web = s->algo == A_GRAINWEB;
        const float glen = V(0) * ms, dens = V(1), dly = V(web ? 4 : 2) * ms;
        float mods[4] = {0, 0, 0, 0};
        if (!web) for (int k = 0; k < 4; k++) mods[k] = sine01(adv(&s->lfo[k], V(7) * (1 + 0.37f * (float)k), fs));
        for (int ch = 0; ch < 2; ch++) {
            float *timer = &s->st[ch][0];
            *timer -= dens / fs;
            if (*timer <= 0) {   /* a new grain */
                *timer += 0.5f + rnd(s);
                for (int g = 0; g < NGR; g++) {
                    Grain *gr = &s->gr[ch][g];
                    if (gr->on) continue;
                    const float st = web ? (rnd(s) < 0.5f ? V(2) : V(3)) : mods[0] * V(4) * 0.01f;
                    const int rev = web && rnd(s) < PC(6);
                    const float ratio = powf(2, st / 12);
                    const float len = glen * (web ? 1 : 1 + 0.25f * mods[2]);
                    const float jitter = web ? rnd(s) * dly * 0.5f : PC(5) * (0.5f + 0.5f * mods[1]) * dly * 0.5f;
                    gr->on = 1; gr->age = 0; gr->len = len;
                    /* the read point's delay changes by inc each sample: forwards at `ratio`, or backwards */
                    gr->inc = rev ? 1 + ratio : 1 - ratio;
                    gr->pos = dly + jitter + 4 + (!rev && ratio > 1 ? (ratio - 1) * len : 0);
                    gr->pan = web ? rnd(s) - 0.5f : mods[3] * PC(6) * 0.5f;
                    break;
                }
            }
            float y = 0;
            for (int g = 0; g < NGR; g++) {
                Grain *gr = &s->gr[ch][g];
                if (!gr->on) continue;
                y += line_read(&s->dl[ch], clampf(gr->pos, 2, (float)s->dl[ch].mask - 4)) * hann01(gr->age / gr->len) * (1 + (ch ? gr->pan : -gr->pan));
                gr->pos += gr->inc;
                if (++gr->age >= gr->len) gr->on = 0;
            }
            y = lp(&s->op[ch][0], y * 1.4f / sqrtf(1 + dens * V(0) * 0.001f), c[20]);
            out[ch] = y;
            line_push(&s->dl[ch], in[ch] + tanh_fast(y * PC(web ? 5 : 3) * 0.7f));
        }
        break;
    }
    case A_GLITCH: case A_STUTTER: {   /* catch a slice into dl2 (as a plain buffer) and replay it */
        const int glitch = s->algo == A_GLITCH;
        float *ph = &s->lfo[0];
        const float before = *ph;
        adv(ph, V(glitch ? 0 : 2), fs);
        float *active = &s->st[0][0], *pos = &s->st[0][1], *len = &s->st[0][2], *left = &s->st[0][3], *dir = &s->st[0][4], *speed = &s->st[0][5];
        for (int ch = 0; ch < 2; ch++) line_push(&s->dl[ch], in[ch]);
        if (*ph < before && rnd(s) < PC(glitch ? 1 : 3)) {
            const int L = (int)clampf(V(glitch ? 4 : 0) * ms, 16, (float)s->dl2[0].mask - 4);
            const int jump = glitch && rnd(s) < PC(6) ? (int)(rnd(s) * 0.5f * fs) : 0;   /* a clock jump: from further back */
            for (int ch = 0; ch < 2; ch++)
                for (int i = 0; i < L; i++) s->dl2[ch].b[i] = line_tap(&s->dl[ch], L + jump - 1 - i);
            *active = 1; *len = (float)L; *pos = 0; *speed = 1;
            *left = (float)L * (glitch ? (float)(1 + (int)(rnd(s) * 4)) : V(1));
            *dir = rnd(s) < PC(glitch ? 5 : 6) ? -1.0f : 1.0f;
        }
        if (*active > 0.5f) {
            const float fade = glitch ? 0.1f : clampf(PC(4), 0.02f, 1);
            const float u = *pos / *len, w = clampf(fminf(u, 1 - u) / (fade * 0.5f + 1e-3f), 0, 1);
            const float at = *dir > 0 ? *pos : *len - 1 - *pos;
            const int i0 = (int)at;
            const float f = at - (float)i0;
            for (int ch = 0; ch < 2; ch++) {
                const float *b = s->dl2[ch].b;
                const int i1 = i0 + 1 < (int)*len ? i0 + 1 : i0;
                out[ch] = (b[i0] + (b[i1] - b[i0]) * f) * w;
            }
            if (!glitch && PC(5) > 0.01f) *speed = fmaxf(0.05f, *speed - PC(5) * 2 / fmaxf(*left, 1));   /* tape stop */
            *pos += *speed;
            if (*pos >= *len) *pos -= *len;
            if ((*left -= 1) <= 0) *active = 0;
        } else { out[0] = in[0]; out[1] = in[1]; }
        if (glitch) {
            const int alias = (int)V(3);
            for (int ch = 0; ch < 2; ch++) {
                if (++s->st[ch][6] >= (float)alias) { s->st[ch][6] = 0; s->st[ch][7] = floorf(out[ch] * c[21] + 0.5f) / c[21]; }
                out[ch] = lp(&s->op[ch][0], s->st[ch][7], c[20]);
            }
        }
        break;
    }
    default: out[0] = out[1] = 0; break;
    }
}

static void slot_set_algo(Slot *s, int algo) { if (algo != s->algo) { slot_clear(s); s->algo = algo; } }

/* one sample through the slot -> its wet sound */
static void slot_run(Slot *s, float l, float r, float *wl, float *wr) {
    const float mix_t = s->algo == A_TRICHORUS && s->v[7] > 0.5f ? 1 : s->mix_n;   /* (Tri Chorus's Vibrato: wet only) */
    s->mix_s += 0.002f * (mix_t - s->mix_s);
    if (!(s->tick++ & 15)) slot_control(s);
    const float in[2] = {l, r};
    float out[2];
    run_algo(s, in, out);
    *wl = out[0];
    *wr = out[1];
}

/* ==== the plugin ============================================================================================= */
typedef struct {
    AEffect fx;
    audioMasterCallback master;
    float norm[kNumParams];
    float fs;
    Slot slot[2];
    int ready;
    float out_s;
    volatile int update_display;
    volatile uint64_t automate;     /* parameters to report back to MPC (bit = index), sent from process() */
    int preset[2];                  /* the preset each slot last loaded (-1: none) */
    int program;                    /* the program last loaded (-1: none) */
    float byp_s[2];                 /* each slot's input, smoothed: 1 playing, 0 bypassed */
    unsigned char cc_high[128];     /* the switch CCs' last state (a press = a value of 64+ after a lower one) */
} Plugin;

static void copy_str(void *dst, const char *src, size_t max) { const size_t n = strnlen(src, max - 1); memcpy(dst, src, n); ((char *)dst)[n] = 0; }
static int algo_of(const Plugin *s, int k) { return option_of(s->norm[k * P_SLOT + P_ALGO], kNumAlgos); }

static void load_defaults(Plugin *s, int k, int algo) {   /* an algorithm was chosen: its own knob settings */
    const Algo *a = &ALGOS[algo];
    for (int i = P_NAME; i <= P_INFO; i++) {   /* the name/info read-outs follow (their value is the algorithm) */
        s->norm[k * P_SLOT + i] = (float)algo / (float)(kNumAlgos - 1);
        s->automate |= 1ull << (k * P_SLOT + i);
    }
    s->norm[k * P_SLOT + P_MIX] = a->mix * 0.01f;
    s->automate |= 1ull << (k * P_SLOT + P_MIX);
    for (int i = 0; i < NK; i++) {
        s->norm[k * P_SLOT + P_K + i] = a->k[i].name ? knob_norm(&a->k[i], a->k[i].def) : 0;
        s->automate |= 1ull << (k * P_SLOT + P_K + i);
    }
}

/* the colour of a slot's page (tools/pedal_looks.h): DrivPdls after its pedal, a few algorithms after theirs, the rest
 * by group. (Quad Rig asks duo_look for its slots.) */
static const unsigned char PEDAL_LOOK_OF[] = {LK_GREEN, LK_GOLD, LK_BLACK, LK_SILVER, LK_RED, LK_BLUE, LK_ORANGE, LK_IVORY,
    LK_IVORY, LK_BLACK, LK_PERIWINKLE, LK_SKY, LK_IVORY, LK_BLACK, LK_YELLOW, LK_IVORY, LK_SILVER, LK_GREEN, LK_PLUM, LK_COPPER,
    LK_YELLOW, LK_YELLOW, LK_GREEN, LK_COPPER, LK_BLACK};
_Static_assert(sizeof PEDAL_LOOK_OF == sizeof PEDALS / sizeof *PEDALS, "a look per pedal");
static int slot_look(const Plugin *s, int k) {
    static const unsigned char GROUP_LOOK[] = {LK_DELAY, LK_DRIVE, LK_PITCH, LK_MOD, LK_REVERB, LK_GRAIN, LK_GRAPHITE};
    const int a = algo_of(s, k);
    switch (a) {
    case A_PEDALS: return PEDAL_LOOK_OF[option_of(s->norm[k * P_SLOT + P_K], N_PEDALS)];
    case A_MIDDRIVE: return LK_GREEN;        /* the green screamer */
    case A_POG: return LK_SILVER;
    case A_TRICHORUS: return LK_PERIWINKLE;
    case A_VIBE: return LK_TURQUOISE;
    default: return GROUP_LOOK[ALGOS[a].group];
    }
}
int duo_look(AEffect *e) { return slot_look((Plugin *)e->object, 0); }

/* presets (presets.h): a preset sets the slot's algorithm, Mix and knobs */
static void load_preset(Plugin *s, int k, int idx) {
    const int base = k * P_SLOT;
    s->norm[base + P_ALGO] = (float)PRESETS[idx].algo / (float)(kNumAlgos - 1);
    load_defaults(s, k, PRESETS[idx].algo);   /* (names, info, and every knob reported back) */
    s->norm[base + P_MIX] = PRESETS[idx].mix;
    for (int i = 0; i < NK; i++) s->norm[base + P_K + i] = PRESETS[idx].k[i];
    s->preset[k] = idx;
    s->norm[kPresetBase + k * 3] = (float)idx / (float)(NPRESETS - 1);
    s->automate |= 1ull << (base + P_ALGO) | 1ull << (kPresetBase + k * 3);
    s->update_display = 1;
}
/* the next / previous preset: within the slot's algorithm and on into the next one's; after the algorithm was
 * changed by hand, the first (or last) preset of the new one */
static void step_preset(Plugin *s, int k, int dir) {
    const int algo = algo_of(s, k), cur = s->preset[k];
    int idx;
    if (cur >= 0 && PRESETS[cur].algo == algo) idx = (cur + dir + NPRESETS) % NPRESETS;
    else {
        idx = -1;
        for (int i = 0; i < NPRESETS; i++) {
            const int j = dir > 0 ? i : NPRESETS - 1 - i;
            if (PRESETS[j].algo == algo) { idx = j; break; }
        }
        if (idx < 0)   /* no presets for this algorithm: the nearest algorithm's */
            for (int i = 0; i < NPRESETS && idx < 0; i++) {
                const int j = dir > 0 ? i : NPRESETS - 1 - i;
                if (dir > 0 ? PRESETS[j].algo > algo : PRESETS[j].algo < algo) idx = j;
            }
        if (idx < 0) idx = dir > 0 ? 0 : NPRESETS - 1;
    }
    load_preset(s, k, idx);
}

/* programs (presets.h, PROGRAMS[]): the H90 factory programs found in a backup, as both slots + the routing */
static void load_program(Plugin *s, int idx) {
    for (int k = 0; k < 2; k++) {
        const ProgSlot *ps = &PROGRAMS[idx].s[k];
        const int base = k * P_SLOT;
        s->norm[base + P_ALGO] = (float)ps->algo / (float)(kNumAlgos - 1);
        load_defaults(s, k, ps->algo);
        s->norm[base + P_MIX] = ps->mix;
        for (int i = 0; i < NK; i++) s->norm[base + P_K + i] = ps->k[i];
        s->preset[k] = -1;   /* (the preset stepper shows the slot's preset by name, if we have it) */
        for (int i = 0; i < NPRESETS; i++) if (PRESETS[i].algo == ps->algo && !strcmp(PRESETS[i].name, ps->name)) { s->preset[k] = i; break; }
        if (s->preset[k] >= 0) s->norm[kPresetBase + k * 3] = (float)s->preset[k] / (float)(NPRESETS - 1);
        s->automate |= 1ull << (base + P_ALGO) | 1ull << (kPresetBase + k * 3);
    }
    s->norm[kRouting] = (float)PROGRAMS[idx].parallel;
    s->program = idx;
    s->norm[kProgBase] = (float)idx / (float)(NPROGRAMS - 1);
    s->automate |= 1ull << kRouting | 1ull << kProgBase;
    s->update_display = 1;
}

static int setup(Plugin *s, float fs) {
    s->ready = 0;
    for (int k = 0; k < 2; k++) slot_free(&s->slot[k]);
    memset(s->slot, 0, sizeof s->slot);
    s->fs = fs;
    const int ok = slot_init(&s->slot[0], fs) && slot_init(&s->slot[1], fs);
    for (int k = 0; k < 2; k++) { s->slot[k].algo = -1; s->slot[k].mix_s = s->norm[k * P_SLOT + P_MIX]; }
    s->ready = ok;
    return ok;
}

static void setParameter(AEffect *e, int32_t i, float v) {
    Plugin *s = (Plugin *)e->object;
    if (i < 0 || i >= kNumParams) return;
    v = clampf(v, 0, 1);
    if (i == kRouting) { s->norm[i] = (float)option_of(v, 2); s->update_display = 1; return; }
    if (i == kOutput) { s->norm[i] = v; return; }
    if (i >= kLookA) return;   /* read-only */
    if (i >= kBypass) { s->norm[i] = v > 0.5f ? 1 : 0; s->update_display = 1; return; }
    if (i >= kProgBase) {   /* as a preset below, over the programs */
        const int w = i - kProgBase;
        if (w) {
            if (v > 0.5f) load_program(s, s->program < 0 ? (w == 1 ? NPROGRAMS - 1 : 0) : (s->program + (w == 1 ? -1 : 1) + NPROGRAMS) % NPROGRAMS);
            s->norm[i] = 0;
            s->automate |= 1ull << i;
            return;
        }
        const float pos = v * (float)(NPROGRAMS - 1), cur = s->program >= 0 ? (float)s->program : s->norm[i] * (float)(NPROGRAMS - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) {
            load_program(s, ((int)lrintf(cur) + (pos > cur ? 1 : -1) + NPROGRAMS) % NPROGRAMS);
            return;
        }
        s->program = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    if (i >= kPresetBase) {
        const int k = (i - kPresetBase) / 3, w = (i - kPresetBase) % 3;
        if (w) {   /* < or >: a momentary button */
            if (v > 0.5f) step_preset(s, k, w == 1 ? -1 : 1);
            s->norm[i] = 0;
            s->automate |= 1ull << i;   /* (its light goes out) */
            return;
        }
        /* the preset itself: a small move (a Q-Link nudge) steps one; an exact value (a project being loaded)
         * only shows which preset it was, without overwriting the knobs saved with it */
        const float pos = v * (float)(NPRESETS - 1), cur = s->preset[k] >= 0 ? (float)s->preset[k] : s->norm[i] * (float)(NPRESETS - 1);
        if (fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1) { step_preset(s, k, pos > cur ? 1 : -1); return; }
        s->preset[k] = (int)lrintf(pos);
        s->norm[i] = v;
        s->update_display = 1;
        return;
    }
    const int k = i / P_SLOT, w = i % P_SLOT;
    if (w >= P_NAME) return;   /* read-only */
    if (w == P_OPEN) { s->norm[i] = v > 0.5f ? 1 : 0; return; }
    if (w == P_ALGO) {   /* a pick from the list (which closes it), or a Q-Link nudge (steps one) */
        const float pos = v * (float)(kNumAlgos - 1), cur = (float)algo_of(s, k);
        const int between = fabsf(pos - roundf(pos)) > 0.001f;
        int opt = between && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
        opt = opt < 0 ? 0 : opt > kNumAlgos - 1 ? kNumAlgos - 1 : opt;
        s->norm[i] = (float)opt / (float)(kNumAlgos - 1);
        if (opt != (int)cur) load_defaults(s, k, opt);
        if (!between && s->norm[k * P_SLOT + P_OPEN] > 0.5f) {
            s->norm[k * P_SLOT + P_OPEN] = 0;
            s->automate |= 1ull << (k * P_SLOT + P_OPEN);
        }
        s->update_display = 1;
        return;
    }
    if (w >= P_K && w < P_K + NK) {   /* a choice or whole-number knob snaps (a Q-Link nudge steps one) */
        const Knob *kn = &ALGOS[algo_of(s, k)].k[w - P_K];
        if (kn->name && (kn->map == M_CHOICE || kn->map == M_INT)) {
            const int n = (int)(kn->hi - kn->lo) + 1;
            if (n > 1) {
                const float pos = v * (float)(n - 1), cur = (float)option_of(s->norm[i], n);
                int opt = n > 2 && fabsf(pos - roundf(pos)) > 0.001f && fabsf(pos - cur) < 1 ? (int)cur + (pos > cur ? 1 : -1) : (int)lrintf(pos);
                opt = opt < 0 ? 0 : opt > n - 1 ? n - 1 : opt;
                v = (float)opt / (float)(n - 1);
            }
        }
    }
    s->norm[i] = v;
}
static float getParameter(AEffect *e, int32_t i) {
    Plugin *s = (Plugin *)e->object;
    return i >= 0 && i < kNumParams ? s->norm[i] : 0;
}

static void fmt_knob(const Knob *k, float n, char *t) {
    if (!k->name) { snprintf(t, 32, " "); return; }
    const float v = knob_value(k, n);
    if (k->map == M_CHOICE) { snprintf(t, 32, "%s", k->choices[(int)v]); return; }
    if (!strcmp(k->unit, "ms") && v >= 1000) { snprintf(t, 32, "%.2f s", v / 1000); return; }
    if (!strcmp(k->unit, "Hz") && v >= 1000) { snprintf(t, 32, "%.1f kHz", v / 1000); return; }
    if (!strcmp(k->unit, "s") && v >= 99.5f && k->hi >= 100) { snprintf(t, 32, "Infinite"); return; }
    if (!strcmp(k->unit, "st") || !strcmp(k->unit, "c")) { snprintf(t, 32, "%+.0f %s", v, k->unit); return; }
    if (!strcmp(k->unit, "dB")) { snprintf(t, 32, "%+.1f dB", v); return; }
    const int pct = !strcmp(k->unit, "%");
    if (k->map == M_INT || pct || fabsf(v) >= 100) snprintf(t, 32, "%.0f%s%s", v, k->unit[0] && !pct ? " " : "", k->unit);
    else snprintf(t, 32, fabsf(v) >= 10 ? "%.1f %s" : "%.2f %s", v, k->unit);
}

static void param_display(Plugin *s, int i, char *t) {
    if (i == kRouting) { snprintf(t, 96, "%s", s->norm[i] > 0.5f ? "Parallel" : "Series"); return; }
    if (i == kOutput) { snprintf(t, 96, "%+.1f dB", lerp(-24, 12, s->norm[i])); return; }
    if (i >= kLookA) { snprintf(t, 96, "%s", PEDAL_LOOKS[slot_look(s, i - kLookA)].name); return; }
    if (i >= kBypass) { snprintf(t, 96, "%s", s->norm[i] > 0.5f ? "Bypassed" : "On"); return; }
    if (i >= kProgBase) {
        const int w = i - kProgBase;
        if (w) snprintf(t, 96, "%s", w == 1 ? "<" : ">");
        else snprintf(t, 96, "%s", s->program >= 0 && s->program < NPROGRAMS ? PROGRAMS[s->program].name : "-");
        return;
    }
    if (i >= kPresetBase) {
        const int k = (i - kPresetBase) / 3, w = (i - kPresetBase) % 3, p = s->preset[k];
        if (w) snprintf(t, 96, "%s", w == 1 ? "<" : ">");
        else if (p >= 0 && p < NPRESETS && PRESETS[p].algo == algo_of(s, k)) snprintf(t, 96, "%s", PRESETS[p].name);
        else snprintf(t, 96, "-");
        return;
    }
    const int k = i / P_SLOT, w = i % P_SLOT;
    const Algo *a = &ALGOS[algo_of(s, k)];
    if (w == P_ALGO) snprintf(t, 96, "%s", a->name);
    else if (w == P_OPEN) snprintf(t, 96, "%s", s->norm[i] > 0.5f ? "Open" : "Closed");
    else if (w == P_MIX) snprintf(t, 96, "%.0f%%", s->norm[i] * 100);
    else if (w < P_NAME) fmt_knob(&a->k[w - P_K], s->norm[i], t);
    else if (w < P_INFO) snprintf(t, 96, "%s", a->k[w - P_NAME].name ? a->k[w - P_NAME].name : " ");
    else snprintf(t, 96, "%s%s%s", GROUP_NAMES[a->group], a->group == G_NONE ? "" : ": ", a->about);
}

static void process(AEffect *e, float **in, float **out, int32_t n) {
#if defined(__arm__)
    uint32_t fpscr;   /* flush denormals (slow on ARMv7's VFP) while we run */
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr | (1u << 24)));
#endif
    Plugin *s = (Plugin *)e->object;
    for (int k = 0; k < 2; k++) {   /* the page's colour follows the slot's effect */
        const float lv = (float)slot_look(s, k) / (float)(N_LOOKS - 1);
        if (s->norm[kLookA + k] != lv) { s->norm[kLookA + k] = lv; s->automate |= 1ull << (kLookA + k); }
    }
    if (s->master) {
        const uint64_t a = s->automate;
        if (a) {
            s->automate &= ~a;
            for (int i = 0; i < kNumParams; i++) if (a & (1ull << i)) s->master(e, audioMasterAutomate, i, 0, NULL, s->norm[i]);
            s->update_display = 1;
        }
        if (s->update_display) { s->update_display = 0; s->master(e, audioMasterUpdateDisplay, 0, 0, NULL, 0); }
    }
    const float *il = in && in[0] ? in[0] : NULL, *ir = in && in[1] ? in[1] : il;
    if (!s->ready) { for (int k = 0; k < n; k++) out[0][k] = out[1][k] = 0; goto done; }
    for (int k = 0; k < 2; k++) {
        Slot *sl = &s->slot[k];
        slot_set_algo(sl, algo_of(s, k));
        sl->mix_n = s->norm[k * P_SLOT + P_MIX];
        for (int j = 0; j < NK; j++) sl->n[j] = s->norm[k * P_SLOT + P_K + j];
    }
    const int parallel = s->norm[kRouting] > 0.5f;
    const float byp_t[2] = {s->norm[kBypass] > 0.5f || s->norm[kBypassA] > 0.5f ? 0.0f : 1.0f,
                            s->norm[kBypass] > 0.5f || s->norm[kBypassB] > 0.5f ? 0.0f : 1.0f};
    const float og = db(lerp(-24, 12, s->norm[kOutput]));
    for (int k = 0; k < n; k++) {
        float l = il ? il[k] : 0, r = ir ? ir[k] : 0;
        if (!finite_bits(l) || l > 8 || l < -8) l = 0;   /* only clean audio in */
        if (!finite_bits(r) || r > 8 || r < -8) r = 0;
        float yl, yr;
        for (int c = 0; c < 2; c++) s->byp_s[c] += 0.003f * (byp_t[c] - s->byp_s[c]);
        if (!parallel) {
            yl = l; yr = r;
            for (int c = 0; c < 2; c++) {
                Slot *sl = &s->slot[c];
                if (sl->algo == A_OFF) continue;
                float wl, wr;
                const float g = s->byp_s[c];   /* (bypassed: it hears nothing, its tail rings on, the dry passes) */
                slot_run(sl, yl * g, yr * g, &wl, &wr);
                const float m = sl->mix_s, dg = lerp(1, m < 0.5f ? 1 : 2 * (1 - m), g), wg = m > 0.5f ? 1 : 2 * m;
                yl = yl * dg + wl * wg;
                yr = yr * dg + wr * wg;
            }
        } else {
            float dg = 0, sum_l = 0, sum_r = 0;
            for (int c = 0; c < 2; c++) {
                Slot *sl = &s->slot[c];
                if (sl->algo == A_OFF) { dg += 0.5f; continue; }
                float wl, wr;
                const float g = s->byp_s[c];
                slot_run(sl, l * g, r * g, &wl, &wr);
                const float m = sl->mix_s;
                dg += 0.5f * lerp(1, m < 0.5f ? 1 : 2 * (1 - m), g);
                const float wg = m > 0.5f ? 1 : 2 * m;
                sum_l += wl * wg; sum_r += wr * wg;
            }
            yl = l * dg + sum_l;
            yr = r * dg + sum_r;
        }
        s->out_s += 0.002f * (og - s->out_s);
        yl *= s->out_s; yr *= s->out_s;
        if (!finite_bits(yl) || !finite_bits(yr) || fabsf(yl) > 63 || fabsf(yr) > 63) {   /* past +36 dB: start over */
            slot_clear(&s->slot[0]); slot_clear(&s->slot[1]);
            yl = yr = 0;
        }
        out[0][k] = clampf(yl, -4, 4);   /* +12 dB ceiling */
        out[1][k] = clampf(yr, -4, 4);
    }
done:
#if defined(__arm__)
    __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
#endif
    return;
}

typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; unsigned char midiData[4];
                 char detune, noteOffVelocity, reserved1, reserved2; } VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[2]; } VstEvents;
enum { effProcessEvents = 25 };

static void set_reported(Plugin *s, int i, float v) { s->norm[i] = v; s->automate |= 1ull << i; s->update_display = 1; }
static void midi(Plugin *s, const unsigned char *m) {   /* (from the host's thread, as setParameter) */
    const int st = m[0] & 0xf0;
    if (st == 0xc0) { if (m[1] < NPROGRAMS) load_program(s, m[1]); return; }
    if (st != 0xb0 || m[1] < CC_BYPASS || m[1] > CC_LAST) return;
    const int cc = m[1], high = m[2] >= 64, press = high && !s->cc_high[cc];
    s->cc_high[cc] = (unsigned char)high;
    if (cc < CC_PROG_DOWN) { set_reported(s, kBypass + cc - CC_BYPASS, (float)high); return; }
    if (!press) return;
    if (cc < CC_PRESET_A_DOWN) {
        const int dir = cc == CC_PROG_DOWN ? -1 : 1;
        load_program(s, s->program < 0 ? (dir < 0 ? NPROGRAMS - 1 : 0) : (s->program + dir + NPROGRAMS) % NPROGRAMS);
    } else step_preset(s, cc >= CC_PRESET_B_DOWN, (cc - CC_PRESET_A_DOWN) % 2 ? 1 : -1);
}

static const char *const SLOT_PARAM_NAMES[P_SLOT] = {"Algorithm", "Algorithm List", "Mix", "Knob 1", "Knob 2", "Knob 3",
    "Knob 4", "Knob 5", "Knob 6", "Knob 7", "Knob 8", "Name 1", "Name 2", "Name 3", "Name 4", "Name 5", "Name 6",
    "Name 7", "Name 8", "Info"};

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t val, void *p, float opt) {
    Plugin *s = (Plugin *)e->object;
    switch (op) {
    case effOpen: return 1;
    case effClose: for (int k = 0; k < 2; k++) slot_free(&s->slot[k]); free(s); return 1;
    case effGetParamName:
        if (idx >= 0 && idx < kNumParams) {
            char t[40];
            const int w = idx % P_SLOT;
            const Knob *kn = idx < kRouting && w >= P_K && w < P_K + NK ? &ALGOS[algo_of(s, idx / P_SLOT)].k[w - P_K] : NULL;
            if (idx >= kLookA) snprintf(t, sizeof t, "%c Look", idx == kLookA ? 'A' : 'B');
            else if (idx >= kBypass) snprintf(t, sizeof t, "%sBypass", idx == kBypass ? "" : idx == kBypassA ? "A " : "B ");
            else if (idx >= kProgBase) snprintf(t, sizeof t, "Program%s", idx == kProgBase ? "" : idx == kProgBase + 1 ? " <" : " >");
            else if (idx >= kPresetBase) snprintf(t, sizeof t, "%c Preset%s", (idx - kPresetBase) / 3 ? 'B' : 'A',
                                             (idx - kPresetBase) % 3 == 0 ? "" : (idx - kPresetBase) % 3 == 1 ? " <" : " >");
            else if (idx == kRouting) snprintf(t, sizeof t, "Routing");
            else if (idx == kOutput) snprintf(t, sizeof t, "Output");
            else if (kn) snprintf(t, sizeof t, "%s", kn->name ? kn->name : "-");   /* the algorithm's own name for it */
            else if (w == P_MIX) snprintf(t, sizeof t, "Mix");
            else snprintf(t, sizeof t, "%c %s", idx < P_SLOT ? 'A' : 'B', SLOT_PARAM_NAMES[w]);
            copy_str(p, t, 32);
        }
        return 1;
    case effGetParamLabel: copy_str(p, "", 8); return 1;
    case effGetParamDisplay:
        if (idx >= 0 && idx < kNumParams) { char t[96]; param_display(s, idx, t); copy_str(p, t, idx < kRouting && idx % P_SLOT == P_INFO ? 64 : 24); }
        return 1;
    case effCanBeAutomated: return idx >= 0 && idx < kNumParams && !(idx < kRouting && idx % P_SLOT >= P_NAME);
    case effGetPlugCategory: return kPlugCategEffect;
    case effGetEffectName: case effGetProductString: copy_str(p, "Duo FX", 32); return 1;
    case effGetVendorString: copy_str(p, "Other Things", 32); return 1;
    case effGetVendorVersion: return 2000;
    case effGetVstVersion: return 2400;
    case effSetSampleRate: if (opt > 1000 && fabsf(opt - s->fs) > 0.5f) setup(s, opt); return 1;
    case effSetBlockSize: return 1;
    case effMainsChanged: if (val) { slot_clear(&s->slot[0]); slot_clear(&s->slot[1]); } return 1;
    case effProcessEvents: {
        const VstEvents *ev = (const VstEvents *)p;
        for (int k = 0; ev && k < ev->numEvents; k++)
            if (ev->events[k] && ev->events[k]->type == 1) midi(s, ev->events[k]->midiData);
        return 1;
    }
    case effCanDo:
        return p && (!strcmp((const char *)p, "receiveVstEvents") || !strcmp((const char *)p, "receiveVstMidiEvent")) ? 1 : -1;
    default: return 0;
    }
}

/* the full-page lists (tools/picklist.h): A's and B's presets (those of the slot's algorithm), the programs, then
 * A's and B's eight knobs (lists 3..18: a knob that is a choice, e.g. DrivPdls' Pedal, lists its choices; any other
 * doesn't open). (Quad Rig calls this for its slots' Duo FX engines too.) */
int duo_pick(AEffect *e, int list, int op, int i, char *out, int max) {
    Plugin *s = (Plugin *)e->object;
    if (list >= 3) {
        const int k = (list - 3) / NK, w = (list - 3) % NK, idx = k * P_SLOT + P_K + w;
        const Knob *kn = &ALGOS[algo_of(s, k)].k[w];
        if (!kn->name || kn->map != M_CHOICE) return op == PL_COUNT ? -1 : op == PL_CURRENT ? -1 : 0;
        const int n = (int)(kn->hi - kn->lo) + 1;
        if (op == PL_COUNT) return n;
        if (op == PL_CURRENT) return (int)knob_value(kn, s->norm[idx]);
        if (op == PL_HEADING) { snprintf(out, (size_t)max, "%c %s", k ? 'B' : 'A', kn->name); return 0; }
        if (i < 0 || i >= n) return 0;
        if (op == PL_NAME) snprintf(out, (size_t)max, "%s", kn->choices[i]);
        else if (op == PL_CHOOSE) {
            s->norm[idx] = n > 1 ? (float)i / (float)(n - 1) : 0;
            s->automate |= 1ull << idx;
            s->update_display = 1;
        }
        return 0;
    }
    if (op == PL_HEADING) return 0;
    if (list == 2) {
        if (op == PL_COUNT) return NPROGRAMS;
        if (op == PL_CURRENT) return s->program;
        if (i < 0 || i >= NPROGRAMS) return 0;
        if (op == PL_NAME) snprintf(out, (size_t)max, "%s", PROGRAMS[i].name);
        else if (op == PL_CHOOSE) load_program(s, i);
        return 0;
    }
    const int k = list, algo = algo_of(s, k);
    int n = 0, cur = -1, at = -1;
    for (int j = 0; j < NPRESETS; j++) {
        if (PRESETS[j].algo != algo) continue;
        if (j == s->preset[k]) cur = n;
        if (n == i) at = j;
        n++;
    }
    if (op == PL_COUNT) return n;
    if (op == PL_CURRENT) return cur;
    if (at < 0) return 0;
    if (op == PL_NAME) snprintf(out, (size_t)max, "%s", PRESETS[at].name);
    else if (op == PL_CHOOSE) load_preset(s, k, at);
    return 0;
}
static const char *const LIST_KEYS[] = {"a_preset", "b_preset", "program", "a_k1", "a_k2", "a_k3", "a_k4", "a_k5", "a_k6", "a_k7",
                                        "a_k8", "b_k1", "b_k2", "b_k3", "b_k4", "b_k5", "b_k6", "b_k7", "b_k8"};
static const char *const LIST_TITLES[] = {"A Presets", "B Presets", "Programs", "A Knob 1", "A Knob 2", "A Knob 3", "A Knob 4",
                                          "A Knob 5", "A Knob 6", "A Knob 7", "A Knob 8", "B Knob 1", "B Knob 2", "B Knob 3",
                                          "B Knob 4", "B Knob 5", "B Knob 6", "B Knob 7", "B Knob 8"};
static const PickLists LISTS = {19, LIST_KEYS, LIST_TITLES, duo_pick, 1};   /* (1: MIDI Ch) */

static AEffect *make(audioMasterCallback master) {
    sine_init();
    Plugin *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->master = master;
    s->norm[P_ALGO] = (float)A_TAPE / (float)(kNumAlgos - 1);            /* A: tape echo into B: hall */
    s->norm[P_SLOT + P_ALGO] = (float)A_HALL / (float)(kNumAlgos - 1);
    load_defaults(s, 0, A_TAPE);
    load_defaults(s, 1, A_HALL);
    s->automate = 0;
    s->norm[kRouting] = 0;
    s->norm[kOutput] = 24.0f / 36;                                       /* 0 dB */
    s->preset[0] = s->preset[1] = -1;
    s->program = -1;
    s->byp_s[0] = s->byp_s[1] = 1;
    s->out_s = 1;
    setup(s, 44100);
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
    e->uniqueID = 0x44554f46;   /* 'DUOF' */
    e->version = 2000;
    return e;
}
__attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
#ifdef PICKLIST_OFF
    return make(master);
#else
    return pl_wrap(master, make, &LISTS);
#endif
}

#ifdef DUO_FX_PARAMS
/* the page's parameter list (params.json), written from the tables above so the two can't drift apart:
 *   cc -DDUO_FX_PARAMS -I<vst2.h dir> duo_fx.c -lm && ./a.out > params.json */
int main(void) {
    for (int a = 0; a < kNumAlgos; a++) {   /* the info line (group: about) must fit effGetParamDisplay's 63 characters */
        char t[200];
        snprintf(t, sizeof t, "%s%s%s", GROUP_NAMES[ALGOS[a].group], ALGOS[a].group == G_NONE ? "" : ": ", ALGOS[a].about);
        if (strlen(t) > 63) { fprintf(stderr, "info too long (%zu): %s\n", strlen(t), t); return 1; }
    }
    printf("{\n  \"name\": \"Duo FX\",\n  \"params\": [\n");
    for (int k = 0; k < 2; k++) {
        const char S = k ? 'B' : 'A', s = k ? 'b' : 'a';
        printf("    {\"key\": \"%c_algo\", \"name\": \"%c Algorithm\", \"options\": [", s, S);
        for (int a = 0; a < kNumAlgos; a++) printf("%s\"%s\"", a ? ", " : "", ALGOS[a].name);
        printf("]},\n");
        printf("    {\"key\": \"%c_algo__open\", \"name\": \"%c Algorithm List\", \"options\": [\"Closed\", \"Open\"]},\n", s, S);
        printf("    {\"key\": \"%c_mix\", \"name\": \"%c Mix\", \"min\": 0, \"max\": 100, \"unit\": \"%%\"},\n", s, S);
        for (int i = 0; i < NK; i++) printf("    {\"key\": \"%c_k%d\", \"name\": \"%c Knob %d\", \"min\": 0, \"max\": 100},\n", s, i + 1, S, i + 1);
        for (int i = 0; i < NK; i++)
            printf("    {\"key\": \"%c_n%d\", \"name\": \"%c Name %d\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"},\n", s, i + 1, S, i + 1);
        printf("    {\"key\": \"%c_info\", \"name\": \"%c Info\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"},\n", s, S);
    }
    printf("    {\"key\": \"routing\", \"name\": \"Routing\", \"options\": [\"Series\", \"Parallel\"]},\n");
    printf("    {\"key\": \"output\", \"name\": \"Output\", \"min\": -24, \"max\": 12, \"default\": 0, \"unit\": \"dB\"},\n");
    for (int k = 0; k < 2; k++) {
        const char S = k ? 'B' : 'A', s = k ? 'b' : 'a';
        printf("    {\"key\": \"%c_preset\", \"name\": \"%c Preset\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"},\n", s, S);
        printf("    {\"key\": \"%c_preset_prev\", \"name\": \"%c Preset <\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n", s, S);
        printf("    {\"key\": \"%c_preset_next\", \"name\": \"%c Preset >\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n", s, S);
    }
    printf("    {\"key\": \"program\", \"name\": \"Program\", \"min\": 0, \"max\": 1, \"display\": \"string\", \"type\": \"stepper\"},\n");
    printf("    {\"key\": \"program_prev\", \"name\": \"Program <\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n");
    printf("    {\"key\": \"program_next\", \"name\": \"Program >\", \"options\": [\"Off\", \"On\"], \"momentary\": true},\n");
    printf("    {\"key\": \"bypass\", \"name\": \"Bypass\", \"options\": [\"On\", \"Bypassed\"]},\n");
    printf("    {\"key\": \"a_bypass\", \"name\": \"A Bypass\", \"options\": [\"On\", \"Bypassed\"]},\n");
    printf("    {\"key\": \"b_bypass\", \"name\": \"B Bypass\", \"options\": [\"On\", \"Bypassed\"]}");
    for (int k = 0; k < 2; k++) {
        printf(",\n    {\"key\": \"%c_look\", \"name\": \"%c Look\", \"options\": [", k ? 'b' : 'a', k ? 'B' : 'A');
        for (int l = 0; l < N_LOOKS; l++) printf("%s\"%s\"", l ? ", " : "", PEDAL_LOOKS[l].name);
        printf("]}");
    }
    pl_params_json(&LISTS, stdout);
    printf("\n  ]\n}\n");
    return 0;
}
#endif

#ifdef DUO_FX_TABLE
/* the algorithm table as JSON, for tools/presets.py:  cc -DDUO_FX_TABLE ... && ./a.out > table.json */
int main(void) {
    printf("[\n");
    for (int a = 0; a < kNumAlgos; a++) {
        printf("  {\"name\": \"%s\", \"group\": %d, \"mix\": %g, \"knobs\": [", ALGOS[a].name, ALGOS[a].group, ALGOS[a].mix);
        for (int i = 0; i < NK; i++) {
            const Knob *k = &ALGOS[a].k[i];
            if (!k->name) { printf("%snull", i ? ", " : ""); continue; }
            printf("%s{\"name\": \"%s\", \"map\": %d, \"lo\": %g, \"hi\": %g, \"def\": %g, \"unit\": \"%s\", \"choices\": [", i ? ", " : "",
                   k->name, k->map, k->lo, k->hi, k->def, k->unit);
            for (int c = 0; k->map == M_CHOICE && c <= (int)k->hi; c++) printf("%s\"%s\"", c ? ", " : "", k->choices[c]);
            printf("]}");
        }
        printf("]}%s\n", a < kNumAlgos - 1 ? "," : "");
    }
    printf("]\n");
    return 0;
}
#endif
