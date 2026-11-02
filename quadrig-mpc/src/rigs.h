/* 0dd Rig's rig presets: whole boards (six slots, the amp's place, tone and level). Some after well-known rigs,
 * some for fun. Effects and their presets are named as Duo FX / Ambient Lab name them (letters and digits compared, any case;
 * quad_test checks every name).
 * The amp model is left alone (it's yours): a rig sets NAM's Input, Gate, Bass, Middle, Treble, Presence.
 * Knob values: 0..1 as on the knob (K_ = leave as the preset has it). Mix: M_ = leave as the preset has it.
 * New rigs go at the end (MIDI Program Change n picks rig n + 1). */
#define K_ (-1.0f)
#define M_ (-1.0f)
#define NOK {K_, K_, K_, K_, K_, K_, K_, K_}
typedef struct { const char *fx, *preset; float mix; float k[8]; } RigSlot;
typedef struct {
    const char *name;
    int amp_pos;                          /* 0 first, n after FX n, 6 last */
    float input, gate, bass, mid, treble, presence;   /* NAM: dB, dB, 0..10 x4 */
    float output;                         /* ours, dB */
    RigSlot s[6];
} Rig;
#define OFF {"Off", NULL, M_, NOK}

static const Rig RIGS[] = {
    /* ---- real-world style rigs ---- */
    {"Plexi Crunch", 1, 0, -100, 5, 6.5f, 6, 6, 0,
     {{"DrivPdls", "Tube Scrmr", M_, {K_, 0.25f, 0.55f, 0.75f, K_, K_, K_, K_}}, {"Room", "Guitar Room", 0.22f, NOK}, OFF, OFF, OFF, OFF}},
    {"Texas Blues", 1, 0, -100, 5.5f, 6, 5.5f, 5.5f, 0,
     {{"DrivPdls", "Klone", M_, {K_, 0.4f, 0.55f, 0.7f, K_, K_, K_, K_}}, {"Spring", "Deluxe", 0.28f, NOK}, OFF, OFF, OFF, OFF}},
    {"Dotted Eighths", 1, 0, -100, 4.5f, 5, 6.5f, 6.5f, -1,
     {{"DrivPdls", "Kng o Tone", M_, {K_, 0.3f, 0.6f, K_, K_, K_, K_, K_}}, {"Digital Delay", "Pristine Digital Delay", 0.4f, {0.68f, 0.38f, K_, K_, 0.15f, K_, K_, K_}},
      {"Vintage Delay", "Where The Avenues Have", 0.25f, NOK}, {"Hall", "Hall", 0.3f, NOK}, OFF, OFF}},
    {"Brown Sound", 2, 3, -70, 5.5f, 6, 6.5f, 7, 0,
     {{"Phaser", "Aint Talkin Bout Pha", M_, NOK}, {"DrivPdls", "Plexi Drv", M_, NOK}, {"Tape Echo", "Classic Slap", 0.22f, NOK},
      {"Plate", "GuitPlate", 0.25f, NOK}, OFF, OFF}},
    {"Floydian Lead", 1, 0, -80, 5, 6, 5.5f, 5.5f, 0,
     {{"DrivPdls", "Big Muf", M_, {K_, 0.6f, 0.45f, K_, K_, K_, K_, K_}}, {"TricraChrs", "Lush Life", 0.3f, NOK}, {"Tape Echo", "Echo", 0.35f, {0.75f, 0.4f, K_, K_, K_, K_, K_, K_}},
      {"Hall", "Carnegie Hall", 0.35f, NOK}, OFF, OFF}},
    {"Surf Spring", 2, -3, -100, 4.5f, 5, 6.5f, 6, 0,
     {{"Spring", "Spicy Spring", 0.55f, {0.6f, K_, K_, K_, 0.7f, K_, K_, K_}}, {"Tremolo", "Tumbleweed Trem", M_, NOK}, OFF, OFF, OFF, OFF}},
    {"80s Studio Clean", 0, -6, -100, 5, 4.5f, 6.5f, 6.5f, 0,
     {{"TricraChrs", "Lush Life", 0.45f, NOK}, {"Digital Delay", "Big Warm Digital", 0.3f, NOK}, {"Hall", "Earth Hall", 0.3f, NOK}, OFF, OFF, OFF}},
    {"Modern Metal", 1, 2, -55, 6, 4, 6, 7, -2,
     {{"DrivPdls", "Tube Scrmr", M_, {K_, 0.05f, 0.65f, 0.85f, K_, K_, K_, K_}}, {"Room", "Boxy Ro0m", 0.12f, NOK}, OFF, OFF, OFF, OFF}},
    {"Grunge", 1, 0, -75, 6, 5.5f, 5.5f, 5, -1,
     {{"DrivPdls", "Rodent", M_, NOK}, {"Chorus", "70s Guitar Chorus", 0.25f, NOK}, {"Room", "Room", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Funk Machine", 2, -3, -90, 5, 5.5f, 6.5f, 6, 0,
     {{"Sculpt3", "Wahcraft", M_, NOK}, {"Phaser", "FunkPhase", M_, NOK}, {"Room", "PCM60Room", 0.18f, NOK}, OFF, OFF, OFF}},
    {"Chicken Pickin", 1, 0, -100, 4.5f, 5.5f, 7, 6.5f, 0,
     {{"DrivPdls", "Blues Drvr", M_, {K_, 0.2f, 0.65f, K_, K_, K_, K_, K_}}, {"Tape Echo", "Classic Slap", 0.3f, NOK}, {"Spring", "Deluxe", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Dub Station", 0, -3, -100, 6.5f, 5, 4, 4.5f, 0,
     {{"Band Delay", "Reggae WahDelay Dark", 0.45f, NOK}, {"Spring", "Dark Spring", 0.35f, NOK}, OFF, OFF, OFF, OFF}},
    {"Jangle 12", 2, -3, -100, 4.5f, 5, 6.5f, 6.5f, 0,
     {{"P0G3", "12 String", M_, NOK}, {"TricraChrs", "Summer", 0.3f, NOK}, {"Tape Echo", "Lennon Slap", 0.25f, NOK}, {"Room", "Guitar Room", 0.2f, NOK}, OFF, OFF}},
    {"Shoegaze Wall", 1, 0, -100, 5, 5, 5.5f, 5, -2,
     {{"DrivPdls", "Big Muf Max", M_, NOK}, {"Reverse", "Reverse Space", 0.4f, NOK}, {"Cloudbrst", "Infinite Swell", 0.55f, NOK}, OFF, OFF, OFF}},
    /* ---- crazy rigs ---- */
    {"Fuzz Synth Beast", 1, 0, -80, 6, 5, 5, 5, -2,
     {{"Data Korrupt", "Fuzz Monster", M_, NOK}, {"Polym00n", "DarkMod", 0.35f, NOK}, {"Plate", "OilDrum", 0.25f, NOK}, OFF, OFF, OFF}},
    {"Glitch Cathedral", 2, -3, -100, 5, 5, 6, 6, -1,
     {{"Stutter", NULL, 0.7f, NOK}, {"Glitch", NULL, 0.5f, NOK}, {"CosmicWb", NULL, 0.45f, NOK}, {"ShimmrVerb", "Mountains", 0.45f, NOK}, OFF, OFF}},
    {"Infinite Halo", 0, -6, -100, 5, 5, 6, 5.5f, -2,
     {{"Mikrocosm", "Haze A", 0.5f, NOK}, {"ShimmrVerb", "Mountains", 0.45f, NOK}, {"Cloudbrst", "Cathedral", 0.5f, NOK}, OFF, OFF, OFF}},
    {"Haunted Tape", 0, -3, -100, 5.5f, 5, 4.5f, 4.5f, -1,
     {{"Tensr", "Slow Reverse", 0.45f, NOK}, {"Moodd MK2", "Ghost Loop", 0.5f, NOK}, {"Wormhol3", "Drowned Lands", 0.4f, NOK}, OFF, OFF, OFF}},
    {"Robot Arps", 1, -3, -100, 5, 5, 6, 6, 0,
     {{"HarPeggiatr", "Old Video Game", M_, NOK}, {"FiltrPong", "WubbaWubbaWub", 0.4f, NOK}, {"Hall", "Planetarium1", 0.3f, NOK}, OFF, OFF, OFF}},
    {"Whale Song", 1, -6, -100, 5.5f, 5, 5, 5, -2,
     {{"HarModulatr", "Munchkin Whales", M_, NOK}, {"Rainbow Mchn", "Magic Rise", 0.4f, NOK}, {"Blackhol3", "Nebula", 0.5f, NOK}, OFF, OFF, OFF}},
    {"Six Pedal Chaos", 1, 0, -80, 5, 5, 5.5f, 5.5f, -3,
     {{"PtchFuzz", "Fripper Fuzz", M_, NOK}, {"Rainbow Mchn", "Broken Octaves", 0.4f, NOK}, {"Partikle", "Pitch Sparks", 0.4f, NOK},
      {"UltraTp", "Batman", 0.35f, NOK}, {"Hedr4", "Infinite Arcade", 0.35f, NOK}, {"MangldVerb", "Oblivion", 0.35f, NOK}}},
    {"Space Bass Organ", 1, -3, -100, 6, 5, 5, 5, -1,
     {{"P0G3", "Organ", M_, NOK}, {"Even-Vib3", "VibraVibe", M_, NOK}, {"Polym00n", "DimensionChorus", 0.35f, NOK}, {"NiteSky", "Night Shimmer", 0.4f, NOK}, OFF, OFF}},
};
#define N_RIGS ((int)(sizeof RIGS / sizeof *RIGS))
