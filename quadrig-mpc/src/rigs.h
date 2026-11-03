/* 0dd Rig's rig presets: whole boards (six slots, the amp's place, tone and level). Some after well-known rigs,
 * some for fun. Effects and their presets are named as Duo FX / Ambient Lab name them (letters and digits compared, any case;
 * quad_test checks every name).
 * The amp: a rig picks the first of your models (and cab IRs) whose folder or name has one of its words (amp, cab);
 * if none has, the amp stays as it is. It also sets NAM's Input, Gate, Bass, Middle, Treble, Presence.
 * Knob values: 0..1 as on the knob (K_ = leave as the preset has it). Mix: M_ = leave as the preset has it.
 * New rigs go at the end (MIDI Program Change n picks rig n + 1). */
#define K_ (-1.0f)
#define M_ (-1.0f)
#define NOK {K_, K_, K_, K_, K_, K_, K_, K_}
typedef struct { const char *fx, *preset; float mix; float k[8]; } RigSlot;
typedef struct {
    const char *name;
    const char *amp, *cab;                /* words for the amp model and cab IR (tried in order on your files' folder +
                                           * name, any case); none found: the amp stays as it is */
    int amp_pos;                          /* 0 first, n after FX n, 6 last */
    float input, gate, bass, mid, treble, presence;   /* NAM: dB, dB, 0..10 x4 */
    float output;                         /* ours, dB */
    RigSlot s[6];
} Rig;
#define OFF {"Off", NULL, M_, NOK}

static const Rig RIGS[] = {
    /* ---- real-world style rigs ---- */
    {"Plexi Crunch", "plexi,1959,jtm,super lead,jcm800,marshall", "greenback,4x12,v30", 1, 0, -100, 5, 6.5f, 6, 6, 0,
     {{"DrivPdls", "Tube Scrmr", M_, {K_, 0.25f, 0.55f, 0.75f, K_, K_, K_, K_}}, {"Room", "Guitar Room", 0.22f, NOK}, OFF, OFF, OFF, OFF}},
    {"Texas Blues", "super reverb,bassman,vibroverb,deluxe,fender", "4x10,2x12,1x12", 1, 0, -100, 5.5f, 6, 5.5f, 5.5f, 0,
     {{"DrivPdls", "Klone", M_, {K_, 0.4f, 0.55f, 0.7f, K_, K_, K_, K_}}, {"Spring", "Deluxe", 0.28f, NOK}, OFF, OFF, OFF, OFF}},
    {"Dotted Eighths", "ac30,vox,deluxe,fender", "blue,alnico,2x12", 1, 0, -100, 4.5f, 5, 6.5f, 6.5f, -1,
     {{"DrivPdls", "Kng o Tone", M_, {K_, 0.3f, 0.6f, K_, K_, K_, K_, K_}}, {"Digital Delay", "Pristine Digital Delay", 0.4f, {0.68f, 0.38f, K_, K_, 0.15f, K_, K_, K_}},
      {"Vintage Delay", "Where The Avenues Have", 0.25f, NOK}, {"Hall", "Hall", 0.3f, NOK}, OFF, OFF}},
    {"Brown Sound", "plexi,5150,evh,1959,marshall", "greenback,4x12", 2, 3, -70, 5.5f, 6, 6.5f, 7, 0,
     {{"Phaser", "Aint Talkin Bout Pha", M_, NOK}, {"DrivPdls", "Plexi Drv", M_, NOK}, {"Tape Echo", "Classic Slap", 0.22f, NOK},
      {"Plate", "GuitPlate", 0.25f, NOK}, OFF, OFF}},
    {"Floydian Lead", "hiwatt,plexi,marshall,twin", "fane,4x12", 1, 0, -80, 5, 6, 5.5f, 5.5f, 0,
     {{"DrivPdls", "Big Muf", M_, {K_, 0.6f, 0.45f, K_, K_, K_, K_, K_}}, {"TricraChrs", "Lush Life", 0.3f, NOK}, {"Tape Echo", "Echo", 0.35f, {0.75f, 0.4f, K_, K_, K_, K_, K_, K_}},
      {"Hall", "Carnegie Hall", 0.35f, NOK}, OFF, OFF}},
    {"Surf Spring", "showman,twin,deluxe,fender", "2x12,1x12", 2, -3, -100, 4.5f, 5, 6.5f, 6, 0,
     {{"Spring", "Spicy Spring", 0.55f, {0.6f, K_, K_, K_, 0.7f, K_, K_, K_}}, {"Tremolo", "Tumbleweed Trem", M_, NOK}, OFF, OFF, OFF, OFF}},
    {"80s Studio Clean", "jc120,jc-120,jazz chorus,roland,twin,fender", "2x12", 0, -6, -100, 5, 4.5f, 6.5f, 6.5f, 0,
     {{"TricraChrs", "Lush Life", 0.45f, NOK}, {"Digital Delay", "Big Warm Digital", 0.3f, NOK}, {"Hall", "Earth Hall", 0.3f, NOK}, OFF, OFF, OFF}},
    {"Modern Metal", "5150,6505,rectifier,recto,mesa,evh,engl,diezel,revv", "v30,4x12", 1, 2, -55, 6, 4, 6, 7, -2,
     {{"DrivPdls", "Tube Scrmr", M_, {K_, 0.05f, 0.65f, 0.85f, K_, K_, K_, K_}}, {"Room", "Boxy Ro0m", 0.12f, NOK}, OFF, OFF, OFF, OFF}},
    {"Grunge", "dual rec,rectifier,jcm,marshall,mesa", "v30,4x12", 1, 0, -75, 6, 5.5f, 5.5f, 5, -1,
     {{"DrivPdls", "Rodent", M_, NOK}, {"Chorus", "70s Guitar Chorus", 0.25f, NOK}, {"Room", "Room", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Funk Machine", "twin,jc120,roland,fender", "2x12", 2, -3, -90, 5, 5.5f, 6.5f, 6, 0,
     {{"Sculpt3", "Wahcraft", M_, NOK}, {"Phaser", "FunkPhase", M_, NOK}, {"Room", "PCM60Room", 0.18f, NOK}, OFF, OFF, OFF}},
    {"Chicken Pickin", "deluxe,twin,princeton,fender", "1x12,2x12", 1, 0, -100, 4.5f, 5.5f, 7, 6.5f, 0,
     {{"DrivPdls", "Blues Drvr", M_, {K_, 0.2f, 0.65f, K_, K_, K_, K_, K_}}, {"Tape Echo", "Classic Slap", 0.3f, NOK}, {"Spring", "Deluxe", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Dub Station", "twin,fender,ampeg,svt", "2x12,1x15", 0, -3, -100, 6.5f, 5, 4, 4.5f, 0,
     {{"Band Delay", "Reggae WahDelay Dark", 0.45f, NOK}, {"Spring", "Dark Spring", 0.35f, NOK}, OFF, OFF, OFF, OFF}},
    {"Jangle 12", "ac30,ac15,vox", "blue,alnico,2x12", 2, -3, -100, 4.5f, 5, 6.5f, 6.5f, 0,
     {{"P0G3", "12 String", M_, NOK}, {"TricraChrs", "Summer", 0.3f, NOK}, {"Tape Echo", "Lennon Slap", 0.25f, NOK}, {"Room", "Guitar Room", 0.2f, NOK}, OFF, OFF}},
    {"Shoegaze Wall", "jcm,marshall,orange,ac30", "4x12", 1, 0, -100, 5, 5, 5.5f, 5, -2,
     {{"DrivPdls", "Big Muf Max", M_, NOK}, {"Reverse", "Reverse Space", 0.4f, NOK}, {"Cloudbrst", "Infinite Swell", 0.55f, NOK}, OFF, OFF, OFF}},
    /* ---- crazy rigs ---- */
    {"Fuzz Synth Beast", "svt,ampeg,orange", "8x10,4x12", 1, 0, -80, 6, 5, 5, 5, -2,
     {{"Data Korrupt", "Fuzz Monster", M_, NOK}, {"Polym00n", "DarkMod", 0.35f, NOK}, {"Plate", "OilDrum", 0.25f, NOK}, OFF, OFF, OFF}},
    {"Glitch Cathedral", "jc120,roland,twin,fender", "2x12", 2, -3, -100, 5, 5, 6, 6, -1,
     {{"Stutter", NULL, 0.7f, NOK}, {"Glitch", NULL, 0.5f, NOK}, {"CosmicWb", NULL, 0.45f, NOK}, {"ShimmrVerb", "Mountains", 0.45f, NOK}, OFF, OFF}},
    {"Infinite Halo", "twin,deluxe,fender", "2x12", 0, -6, -100, 5, 5, 6, 5.5f, -2,
     {{"Mikrocosm", "Haze A", 0.5f, NOK}, {"ShimmrVerb", "Mountains", 0.45f, NOK}, {"Cloudbrst", "Cathedral", 0.5f, NOK}, OFF, OFF, OFF}},
    {"Haunted Tape", "tweed,champ,princeton,deluxe", "1x12,1x10", 0, -3, -100, 5.5f, 5, 4.5f, 4.5f, -1,
     {{"Tensr", "Slow Reverse", 0.45f, NOK}, {"Moodd MK2", "Ghost Loop", 0.5f, NOK}, {"Wormhol3", "Drowned Lands", 0.4f, NOK}, OFF, OFF, OFF}},
    {"Robot Arps", "jc120,roland,twin", "2x12", 1, -3, -100, 5, 5, 6, 6, 0,
     {{"HarPeggiatr", "Old Video Game", M_, NOK}, {"FiltrPong", "WubbaWubbaWub", 0.4f, NOK}, {"Hall", "Planetarium1", 0.3f, NOK}, OFF, OFF, OFF}},
    {"Whale Song", "ac30,vox,twin", "2x12", 1, -6, -100, 5.5f, 5, 5, 5, -2,
     {{"HarModulatr", "Munchkin Whales", M_, NOK}, {"Rainbow Mchn", "Magic Rise", 0.4f, NOK}, {"Blackhol3", "Nebula", 0.5f, NOK}, OFF, OFF, OFF}},
    {"Six Pedal Chaos", "orange,marshall,mesa", "4x12", 1, 0, -80, 5, 5, 5.5f, 5.5f, -3,
     {{"PtchFuzz", "Fripper Fuzz", M_, NOK}, {"Rainbow Mchn", "Broken Octaves", 0.4f, NOK}, {"Partikle", "Pitch Sparks", 0.4f, NOK},
      {"UltraTp", "Batman", 0.35f, NOK}, {"Hedr4", "Infinite Arcade", 0.35f, NOK}, {"MangldVerb", "Oblivion", 0.35f, NOK}}},
    {"Space Bass Organ", "svt,ampeg,darkglass,twin", "8x10,4x10", 1, -3, -100, 6, 5, 5, 5, -1,
     {{"P0G3", "Organ", M_, NOK}, {"Even-Vib3", "VibraVibe", M_, NOK}, {"Polym00n", "DimensionChorus", 0.35f, NOK}, {"NiteSky", "Night Shimmer", 0.4f, NOK}, OFF, OFF}},
    /* ---- more (Nov 2026, the user's ask: "make a bunch more") ---- */
    {"Cali Lead", "mark iv,mark v,boogie,mesa", "v30,4x12", 1, 0, -75, 5, 5.5f, 6, 6, -1,
     {{"DrivPdls", "Tube Scrmr", M_, {K_, 0.2f, 0.55f, 0.75f, K_, K_, K_, K_}}, {"Digital Delay", "Big Nice Day", 0.25f, NOK}, {"Plate", "GuitPlate", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Stadium 80s Lead", "jcm800,jcm,5150,marshall", "4x12,v30", 1, 2, -70, 5.5f, 5, 6.5f, 6.5f, -2,
     {{"DrivPdls", "0DR-l", M_, NOK}, {"Flanger", "ClassicFlangeSweep", 0.35f, NOK}, {"Digital Delay", "Big Warm Digital", 0.3f, NOK}, {"Hall", "Hall", 0.25f, NOK}, OFF, OFF}},
    {"Fuzz and Vibe", "plexi,1959,marshall", "greenback,4x12", 2, 0, -100, 5.5f, 6, 5.5f, 5.5f, -1,
     {{"DrivPdls", "Fuz Face", M_, NOK}, {"Even-Vib3", "Widevibe", M_, NOK}, {"Room", "Room", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Tone Lord", "dumble,ods,two rock,tworock", "2x12", 1, 0, -100, 5, 6, 5.5f, 5, 0,
     {{"DrivPdls", "Zendrv", M_, NOK}, {"Tape Echo", "Echo", 0.25f, NOK}, {"Plate", "Plate", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Country Twang", "twin,deluxe,fender", "2x12", 2, 0, -100, 4.5f, 5.5f, 7, 7, 0,
     {{"Ducked Delay", "Country Compressor", M_, NOK}, {"DrivPdls", "EP Boostr", M_, NOK}, {"Tape Echo", "Multi Slap", 0.25f, NOK}, {"Spring", "RW Tank", 0.25f, NOK}, OFF, OFF}},
    {"Djent Machine", "5150,6505,revv,engl,diezel,mesa", "v30,4x12", 1, 2, -50, 5.5f, 4.5f, 6.5f, 7, -2,
     {{"DrivPdls", "0CD", M_, {K_, 0.1f, 0.6f, 0.85f, K_, K_, K_, K_}}, {"Room", "Room 667", 0.1f, NOK}, OFF, OFF, OFF, OFF}},
    {"Doom Sludge", "orange,rockerverb,sunn,ampeg", "4x12", 2, 0, -70, 7, 4.5f, 4.5f, 4.5f, -2,
     {{"DrivPdls", "Big Muf Max", M_, NOK}, {"Octaver", "OctoFuzzver", M_, NOK}, {"MangldVerb", "DoomDrive", 0.3f, NOK}, OFF, OFF, OFF}},
    {"Indie Shimmer", "ac30,vox,deluxe", "blue,alnico,2x12", 0, -3, -100, 4.5f, 5, 6.5f, 6, 0,
     {{"TricraChrs", "Slow Swirl", 0.4f, NOK}, {"Vintage Delay", "Ambient Vintage", 0.3f, NOK}, {"ShimmrVerb", "Shimmer", 0.35f, NOK}, OFF, OFF, OFF}},
    {"Post-Rock Swell", "ac30,twin,fender", "2x12", 1, -3, -100, 5, 5, 6, 5.5f, -1,
     {{"DrivPdls", "Mornin Glo", M_, NOK}, {"MultiTp", "Big SwellPad1", 0.4f, NOK}, {"Blackhol3", "Pulsar", 0.45f, NOK}, OFF, OFF, OFF}},
    {"Reggae Skank", "twin,jc120,fender", "2x12", 0, -3, -100, 4, 5.5f, 6.5f, 6, 0,
     {{"Band Delay", "Wah Echo1", 0.3f, NOK}, {"Spring", "Spring", 0.25f, NOK}, OFF, OFF, OFF, OFF}},
    {"Spaghetti Western", "twin,deluxe,fender", "2x12", 0, -3, -100, 5, 5, 6.5f, 6, 0,
     {{"Tremolo", "I Walk Alone", M_, NOK}, {"Tape Echo", "Echo", 0.35f, NOK}, {"Spring", "BigBalls", 0.35f, NOK}, OFF, OFF, OFF}},
    {"Slide Blues", "tweed,bassman,deluxe,fender", "4x10,1x12", 1, 0, -100, 5.5f, 6, 5.5f, 5.5f, 0,
     {{"DrivPdls", "Swt Honey", M_, NOK}, {"Tape Echo", "Classic Slap", 0.25f, NOK}, {"Plate", "Plate", 0.2f, NOK}, OFF, OFF, OFF}},
    {"Drop Tune Metal", "rectifier,dual rec,mesa,5150", "v30,4x12", 2, 2, -55, 6, 4, 6, 6.5f, -2,
     {{"PitchFlx", "Pitch Flex -2nd", M_, NOK}, {"DrivPdls", "Tube Scrmr", M_, {K_, 0.1f, 0.6f, 0.8f, K_, K_, K_, K_}}, {"Room", "Boxy Ro0m", 0.12f, NOK}, OFF, OFF, OFF}},
    {"Synthwave Chorus", "jc120,jc-120,roland", "2x12", 0, -3, -100, 5, 4.5f, 6.5f, 6.5f, 0,
     {{"Chorus", "Big Clone", 0.5f, NOK}, {"Digital Delay", "Dual Big Warm Digital", 0.3f, NOK}, {"Hall", "1985 Damage", 0.3f, NOK}, OFF, OFF, OFF}},
    {"Lo-Fi Bedroom", "champ,princeton,tweed", "1x10,1x12", 1, -3, -100, 5, 5.5f, 4.5f, 4, -1,
     {{"Sculpt3", "Little Speaker", M_, NOK}, {"Even-Vib3", "Warped Vinyl", M_, NOK}, {"Vintage Delay", "Lo-Rez Trail", 0.3f, NOK}, {"SP2O16", "Bedroom", 0.25f, NOK}, OFF, OFF}},
    {"Harmony Twin Lead", "jcm800,plexi,marshall", "4x12", 1, 0, -75, 5, 6, 6, 6, -1,
     {{"DrivPdls", "Tube Scrmr", M_, NOK}, {"Diatonik", "Delayed 3rd and 5th", 0.45f, NOK}, {"Digital Delay", "TwinDelay", 0.25f, NOK}, {"Hall", "Hall", 0.2f, NOK}, OFF, OFF}},
    {"Bass DI Punch", "svt,ampeg,darkglass", "8x10,4x10", 1, 0, -100, 6, 5, 5, 5.5f, 0,
     {{"Sculpt3", "Sculpt3 Da Bass", M_, NOK}, {"Room", "Room", 0.08f, NOK}, OFF, OFF, OFF, OFF}},
    {"Bass Synth Funk", "svt,ampeg,darkglass", "8x10,4x10", 1, 0, -100, 6, 5, 5.5f, 5, 0,
     {{"PolySynt", "Synth Wah Bass", M_, NOK}, {"Room", "PCM60Room", 0.1f, NOK}, OFF, OFF, OFF, OFF}},
    {"Reverse Dimension", "ac30,vox,twin", "2x12", 0, -3, -100, 5, 5, 5.5f, 5, -1,
     {{"Reverse", "Backwards Riffs", 0.5f, NOK}, {"Wormhol3", "Kessel Run", 0.4f, NOK}, OFF, OFF, OFF, OFF}},
    {"Octave Cathedral", "twin,deluxe,fender", "2x12", 1, -3, -100, 5, 5, 6, 5.5f, -2,
     {{"P0G3", "Shimmer Up", M_, NOK}, {"Crystalz", "Classic Crystalz", 0.4f, NOK}, {"Cloudbrst", "Choral Bloom", 0.45f, NOK}, OFF, OFF, OFF}},
    {"8-Bit Arcade", "jc120,roland,twin", "2x12", 1, -3, -100, 5, 5, 6, 6, -1,
     {{"Data Korrupt", "8-Bit Choir", M_, NOK}, {"Stutter", NULL, 0.6f, NOK}, {"Vintage Delay", "Lo-Rez Trail", 0.3f, NOK}, OFF, OFF, OFF}},
    {"Underwater Radio", "champ,princeton,deluxe", "1x12", 0, -3, -100, 5, 5, 4.5f, 4.5f, -1,
     {{"Tensr", "Under Water", 0.5f, NOK}, {"Partikle", "Broken Radio", 0.45f, NOK}, {"Aftrneath", "Under Ice", 0.45f, NOK}, OFF, OFF, OFF}},
    {"Alien Choir", "twin,jc120,fender", "2x12", 0, -3, -100, 5, 5, 6, 6, -2,
     {{"Quadravx", "Anthem", 0.5f, NOK}, {"Resonator", "Thors Bells", 0.35f, NOK}, {"ShimmrVerb", "Neros Ascent", 0.4f, NOK}, OFF, OFF, OFF}},
    {"Bubble Machine", "ac30,vox,twin", "2x12", 1, -3, -100, 5, 5, 6, 6, -1,
     {{"HarPeggiatr", "Bubbles", M_, NOK}, {"InstFlangr", "Fairy Wings", 0.45f, NOK}, {"Polym00n", "Elevate", 0.35f, NOK}, OFF, OFF, OFF}},
    {"Tape Graveyard", "tweed,champ,deluxe", "1x12", 0, -3, -100, 5.5f, 5, 4.5f, 4, -2,
     {{"Moodd MK2", "Crushed Tape", 0.5f, NOK}, {"Tape Echo", "TapeDrone", 0.45f, NOK}, {"Blackhol3", "Dark Matter", 0.45f, NOK}, OFF, OFF, OFF}},
    {"Wah Robot", "jcm,marshall,plexi", "4x12", 1, 0, -80, 5, 5.5f, 6, 6, -1,
     {{"Sculpt3", "Wahvolver", M_, NOK}, {"PolySynt", "Laser Gun", M_, NOK}, {"FiltrPong", "Science Museum", 0.4f, NOK}, OFF, OFF, OFF}},
    {"Seasick Steamboat", "deluxe,princeton,fender", "1x12,2x12", 0, -3, -100, 5, 5, 5.5f, 5, -1,
     {{"Rainbow Mchn", "Seasick Shift", 0.5f, NOK}, {"Aftrneath", "Seasick", 0.45f, NOK}, {"Tremolo", "Pulsing Trem", M_, NOK}, OFF, OFF, OFF}},
    {"Galaxy Brain", "orange,marshall,ac30", "4x12,2x12", 1, 0, -80, 5, 5, 5.5f, 5.5f, -3,
     {{"DrivPdls", "Plumz", M_, NOK}, {"Mikrocosm", "Glide A", 0.4f, NOK}, {"Hedr4", "Mirror Glide", 0.35f, NOK},
      {"NiteSky", "Star Steps", 0.35f, NOK}, {"UltraTp", "UltraSwell", 0.3f, NOK}, {"Blackhol3", "Centaurus", 0.4f, NOK}}},
    {"Infinite Drone", "twin,fender,deluxe", "2x12", 0, -6, -100, 5.5f, 5, 5, 5, -2,
     {{"Mikrocosm", "Tunnel A", 0.5f, NOK}, {"Cloudbrst", "Endless", 0.55f, NOK}, {"Blackhol3", "AmeBlackDrone", 0.5f, NOK}, OFF, OFF, OFF}},
};
#define N_RIGS ((int)(sizeof RIGS / sizeof *RIGS))
