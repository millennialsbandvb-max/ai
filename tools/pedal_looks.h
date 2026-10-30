/* pedal_looks.h: the enclosure colours a Duo FX / Quad Rig effect page takes, after the pedal its effect is like.
 * The plugin reports which one as a read-only option parameter (Duo: a_look / b_look; Quad Rig: f1_look..f4_look);
 * the page (tools/pedal_looks.py) has a background per look, shown with when=<param>:<look>. This table is the one
 * source: pedal_looks.py reads the names and colours from it (keep one entry per line, in this form).
 * Colours stay deep enough for the pages' light text. Append new looks at the end. */
#pragma once

typedef struct { const char *name, *hi, *lo; } PedalLook;   /* top and bottom of the enclosure (RRGGBB) */
enum {
    LK_GRAPHITE, LK_GREEN, LK_GOLD, LK_BLACK, LK_SILVER, LK_RED, LK_BLUE, LK_ORANGE, LK_IVORY, LK_PERIWINKLE,
    LK_YELLOW, LK_PLUM, LK_DELAY, LK_REVERB, LK_MOD, LK_PITCH, LK_DRIVE, LK_GRAIN, LK_TEAL, LK_PURPLE, LK_SKY,
    LK_TURQUOISE, LK_MAGENTA, LK_COPPER, N_LOOKS
};
static const PedalLook PEDAL_LOOKS[N_LOOKS] = {
    {"Graphite", "4a4d55", "1c1d22"},
    {"Green", "2f8a45", "123d1c"},
    {"Gold", "b48a2c", "5a420f"},
    {"Black", "3a3a3c", "0c0c0e"},
    {"Silver", "8d939c", "434850"},
    {"Red", "b02522", "52100e"},
    {"Blue", "2459b8", "0f285a"},
    {"Orange", "d8661c", "7a3108"},
    {"Ivory", "a39f91", "5a574d"},
    {"Periwinkle", "5d7fbe", "26395f"},
    {"Yellow", "c9a020", "6a500c"},
    {"Plum", "7a3270", "36122f"},
    {"Delay", "2a4f8a", "0f1f3d"},
    {"Reverb", "4b3a8c", "1d153f"},
    {"Mod", "1f7a73", "0b3532"},
    {"Pitch", "8a2a6b", "3a0f2c"},
    {"Drive", "8a2424", "3a0c0c"},
    {"Grain", "8a5424", "3c220c"},
    {"Teal", "2a8a8a", "0f3b3d"},
    {"Purple", "5b3aa0", "241447"},
    {"Sky", "4f8fc9", "1d4266"},
    {"Turquoise", "1f9a8f", "0b4440"},
    {"Magenta", "a8297a", "4a0d34"},
    {"Copper", "a8643a", "4a2814"},
};
