// PocketBeat for M5StickS3 (ESP32-S3 + BMI270)
// Wearable foot drum: stomp = kick, swing right (or hard stomp) = snare, hi-hat follows your tempo.
// - Detection runs on the board; settings are kept in flash.
// - Sound: the board's own speaker, Bluetooth MIDI (GarageBand etc.), and/or the phone via the tuning page.
// - Tuning page: join Wi-Fi "PocketBeat" and open http://192.168.4.1
//
// Libraries (Arduino Library Manager): M5Unified, ESP Async WebServer (ESP32Async), Async TCP (ESP32Async),
//                                      BLE-MIDI (lathoub), NimBLE-Arduino 1.4.x
// Board: ESP32S3 Dev Module, Flash 8MB, PSRAM: QSPI, USB CDC on boot: enabled, partition: 8MB with spiffs

#include <M5Unified.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32_NimBLE.h>
#if ARDUINO_USB_MODE == 0   // USB-OTG (TinyUSB) build: native USB MIDI
#include "USB.h"
#include "USBMIDI.h"
USBMIDI usbMidi;
#define HAVE_USB_MIDI 1
#endif
#ifndef PB_LITE
#include "samples.h"
#endif
#include "web_assets.h"
#include "pb_types.h"

#include "version.h"
static const char* OTA_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/pocketbeat_sticks3.app.bin";
static const char* OTA_VER_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/version.txt";
static const char* OTHER_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/footboard_sticks3.app.bin";   // "Switch to Footboard"
static const char* OTHER_VER_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/footboard_version.txt";

// ---------------- Settings (persisted) ----------------
struct Cfg {
  float thr = 1.8f;      // stomp threshold: impact above this (g) = kick, g (linear accel magnitude)
  float sthr = 3.5f;     // stomp line in tap/stomp modes: harder than this (g) = stomp
  float kthr = 1.8f;     // kick threshold in bounce/stomp/lift mode; lighter landings are hats
  float swthr = 200.0f;  // swing threshold: rotation along the learned swing (deg/s) = snare
  int gap = 120;         // ms between hits
  int snare = 8;         // mode: 8 = heel kick / toe snare (default), 7 = toe kick / heel snare, 6 = stomp kick / swing snare, 2 = tap kick / stomp snare, 1 = tap snare / stomp kick. 0 swing right, 1 tap=snare/stomp=kick, 2 tap=kick/stomp=snare, 3 high lift, 4 bounce=hat/stomp=kick/lift=snare
  float lthr = 35.0f;    // lift threshold, degrees of rotation since the foot last went still
  int kit = 0;           // 0 acoustic, 1 electro
  int out = 0;           // 0 board speaker, 1 phone, 2 both
  int quant = 2;         // 0 off, 2 eighths, 4 sixteenths
  int hat = 2;           // 0 off, 1 quarters, 2 eighths (auto hats)
  int vol = 255;         // speaker volume 0-255
  float rx = 0, ry = 0, rz = 0; bool cal = false;  // "right" direction in sensor frame
  float kx = 0, ky = 0, kz = 0; bool kcal = false; // learned kick direction (Direction mode)
  float sx = 0, sy = 0, sz = 0; bool scal = false; // learned snare direction (Direction mode)
  char ssid[33] = ""; char pass[65] = "";          // home Wi-Fi for GitHub updates
  float lf[6] = {0}; float rf[6] = {0};            // learned toe (lf) / heel (rf) signatures: rotation just before impact
  bool lcal = false, rcal = false;
  int legSwap = 0;                                 // 0: left = kick, right = snare; 1: swapped
  int sigVer = 0;                                  // 2 = leg signatures use the pre-stomp tilt
  float tthr = 15.0f;                              // toe must have dropped at least this many degrees to count as a toe
  int tlock = 0;                                   // 0 = tempo follows the player, 1 = learned from the first 8 hits, held until 2 bars of silence
  float bpm = 100.0f;                              // locked tempo (unused since count-in lock)
  int shk = 0;                                     // Shaker mode sound: 0 shaker, 1 maracas, 2 tambourine
  int ohat = 1;                                    // auto hats open when the toe is raised
  float othr = 20.0f;                              // ...past this many degrees from flat
  float fx = 0, fy = 0, fz = 0; bool flatSet = false;   // gravity direction with the foot flat
  float klvl = 1.2f, slvl = 1.2f;                  // heel/toe modes: minimum impact (g) for a kick and for a snare
  float toeSig[11] = {0}, heelSig[11] = {0};       // learned toe / heel: rot[3], dir[3], reb[3], rise ms, foot angle
  float swang = 25.0f;                             // stomp/swing mode: degrees the foot must swing to play the snare
  float ux = 0, uy = 0, uz = 0; bool upSet = false; // which way gravity moves when the toe is raised (learned after Set flat)
  int kroot = 48;                                  // foot keyboard: lowest note (MIDI number, 48 = C3)
  float krange = 36.0f;                            // foot keyboard: degrees of swivel left / right of centre for the ends of the range
  int kflip = 1;                                   // foot keyboard: swap which way is up
  float kflat = 10.0f;                             // foot keyboard: tilt from flat that releases the note (deg)
  int kspan = 18;                                  // foot keyboard: semitones from lowest to highest note (18 = 1.5 octaves)
  float kdeg = 4.0f;                               // foot keyboard: degrees of swivel per note
  bool kEdge = false; float kLo = 0, kHi = 0;      // foot keyboard: swivel positions of the lowest and highest note, when set
  float zdeg = 15.0f;                              // swivel kit: swivel past this (deg) from centre = snare (out) / open hat (in)
  int zflip = 0;                                   // swivel kit: swap which way is out
} cfg;
Preferences prefs;

// Home Wi-Fi is also kept in a shared "wifi" store, so PocketBeat and Footboard can each use it after a switch.
void syncWifi() {
  Preferences w; w.begin("wifi", false);
  if (cfg.ssid[0]) { if (w.getString("ssid", "") != String(cfg.ssid) || w.getString("pass", "") != String(cfg.pass)) { w.putString("ssid", cfg.ssid); w.putString("pass", cfg.pass); } }
  else { String s = w.getString("ssid", ""), p = w.getString("pass", ""); strlcpy(cfg.ssid, s.c_str(), sizeof cfg.ssid); strlcpy(cfg.pass, p.c_str(), sizeof cfg.pass); }
  w.end();
}
void saveCfg() {
  prefs.begin("pb", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
  if (cfg.ssid[0]) syncWifi();
}
void loadCfg() {
  prefs.begin("pb", true);
  size_t n = prefs.getBytesLength("cfg");
  if (n > 0 && n <= sizeof(cfg)) prefs.getBytes("cfg", &cfg, n);   // older builds stored fewer fields; new ones keep defaults
  if (cfg.sigVer < 3) {    // settings from before the shoe-mode revamp: thresholds and calibration meant other things
    Cfg d; cfg.thr = d.thr; cfg.sthr = 3.5f; cfg.swthr = d.swthr; cfg.gap = d.gap; cfg.cal = false; cfg.snare = 6; cfg.sigVer = 3;
  }
  if (cfg.sigVer < 4) { cfg.lcal = false; cfg.rcal = false; cfg.sigVer = 4; cfg.snare = 8; cfg.thr = 1.2f; }
  if (cfg.sigVer < 5) { cfg.tthr = 15.0f; cfg.sigVer = 5; }
  if (cfg.sigVer < 6) { cfg.lcal = false; cfg.rcal = false; cfg.sigVer = 6; }   // toe/heel now learn five measurements   // lf/rf now hold toe/heel signatures; start in heel/toe mode
  if (cfg.sigVer < 7) { if (n > 0) cfg.kflip = cfg.kflip ? 0 : 1; cfg.sigVer = 7; }   // keyboard pitch direction was backwards: flip whatever was set
  if (cfg.snare != 1 && cfg.snare != 2 && cfg.snare != 6 && cfg.snare != 7 && cfg.snare != 8 && cfg.snare != 9 && cfg.snare != 10) cfg.snare = 8;
  prefs.end();
  syncWifi();
}

volatile int otaRequest = 0;   // 1 = check+update from GitHub (run from loop, not from the websocket task)

// ---------------- Outputs ----------------
BLEMIDI_CREATE_INSTANCE("PocketBeat", MIDI);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
bool bleConnected = false;

const char* kindName[] = { "kick", "snare", "hat", "tap", "shaker", "hatOpen" };
const char* lastKind = "";
float hitAngle = -999;   // signed toe tilt (or toe drop angle) reported with the next hit (deg)
char hitVotes[7] = "";  // per-measurement toe/heel votes reported with the next hit
float hitToeUp = -1, hitHeelUp = -1;   // lifts before the hit, reported with it
const uint8_t midiNote[] = { 36, 38, 42, 0, 82, 46 };   // GM drums: kick, snare, closed hat (tap: none), shaker (set per cfg.shk)
const uint8_t shakeNote[] = { 82, 70, 54 };          // GM: Shaker, Maracas, Tambourine

void playBoard(Kind k, float vel) {
#ifdef PB_LITE
  return;                    // lite build: no samples, no speaker (Bluetooth MIDI + phone only)
#else
  if (cfg.out == 1) return;  // phone only
  const Kit& kt = KITS[cfg.kit];
  const int16_t* d = k == KICK ? kt.kick : k == SNARE ? kt.snare : kt.hat;
  uint32_t n = k == KICK ? kt.nKick : k == SNARE ? kt.nSnare : kt.nHat;
  if (k == SHAKE) {
    d = cfg.shk == 2 ? SMP_tamb : SMP_shaker; n = cfg.shk == 2 ? sizeof(SMP_tamb) / 2 : sizeof(SMP_shaker) / 2;
    M5.Speaker.setChannelVolume(3, (uint8_t)(cfg.vol * (0.15f + 0.85f * vel)));
    M5.Speaker.playRaw(d, n, SMP_RATE, false, 1, 3, true);
    return;
  }
  if (k == OHAT) {
    d = cfg.kit == 1 ? SMP_electro_hatOpen : SMP_acoustic_hatOpen;
    n = cfg.kit == 1 ? sizeof(SMP_electro_hatOpen) / 2 : sizeof(SMP_acoustic_hatOpen) / 2;
    M5.Speaker.setChannelVolume(HAT, (uint8_t)(cfg.vol * 0.6f * (0.7f + 0.3f * vel)));
    M5.Speaker.playRaw(d, n, SMP_RATE, false, 1, HAT, true);   // same channel as closed hats: a closed hat cuts it off
    return;
  }
  int ch = (int)k;  // one speaker channel per drum, so a new hit cuts the old one
  M5.Speaker.setChannelVolume(ch, (uint8_t)(cfg.vol * (k == HAT ? 0.6f : 1.0f) * (0.7f + 0.3f * vel)));
  M5.Speaker.playRaw(d, n, SMP_RATE, false, 1, ch, true);
#endif
}
// ---------------- Foot keyboard ----------------
int keyNote = -1;            // note currently held (MIDI number), -1 = none
float keyQx = 0, keyQy = 0, keyQz = 1;   // foot orientation (gravity direction) when the note started
uint32_t keyOnAt = 0;                    // when the note started
float keyTip = 0;                        // how far the foot has tipped from that position (deg)
bool loSet = false, hiSet = false;       // which keyboard edges have been set since the last clear
uint32_t keyLiftSince = 0;   // when the foot started to leave flat
static const char* NOTE_NAMES[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
void keyName(int n, char* out) { sprintf(out, "%s%d", NOTE_NAMES[n % 12], n / 12 - 1); }
void keyOff() {
  if (keyNote < 0) return;
  if (bleConnected) MIDI.sendNoteOff(keyNote, 0, 1);
#ifdef HAVE_USB_MIDI
  usbMidi.noteOff(keyNote, 0, 1);
#endif
#ifndef PB_LITE
  M5.Speaker.stop(5);
#endif
  char b[64]; snprintf(b, sizeof b, "{\"key\":%d,\"on\":0,\"tip\":%.0f}", keyNote, keyTip); ws.textAll(b);
  keyNote = -1;
}
void keyOn(int note, float vel, float tilt) {
  if (keyNote >= 0) { keyTip = -1; keyOff(); }   // replaced by the new note
  keyNote = note;
  uint8_t v = (uint8_t)constrain(20 + vel * 107, 1, 127);
  if (bleConnected) MIDI.sendNoteOn(note, v, 1);
#ifdef HAVE_USB_MIDI
  usbMidi.noteOn(note, v, 1);
#endif
#ifndef PB_LITE
  if (cfg.out != 1) {
    M5.Speaker.setChannelVolume(5, (uint8_t)(cfg.vol * (0.3f + 0.7f * vel)));
    M5.Speaker.tone(440.0f * powf(2.0f, (note - 69) / 12.0f), 10000, 5, true);   // sounds until the foot lifts
  }
#endif
  char nm[8]; keyName(note, nm);
  char b[96]; snprintf(b, sizeof b, "{\"key\":%d,\"on\":1,\"v\":%.2f,\"name\":\"%s\",\"tilt\":%.0f}", note, vel, nm, tilt); ws.textAll(b);
}
// Which note a swivel picks. Each note is kdeg degrees wide and the range is centred on keyMid().
// Setting the edges fixes the centre (their midpoint), the direction (low end -> high end) and an
// initial kdeg that fits between them; kdeg and kspan can still be changed afterwards.
float keyHalf() { return cfg.kspan * cfg.kdeg / 2; }
float keyMid() { return cfg.kEdge ? (cfg.kLo + cfg.kHi) / 2 : 0; }
int keyForTilt(float a) {
  float dir = (cfg.kEdge ? (cfg.kHi >= cfg.kLo ? 1.0f : -1.0f) : 1.0f) * (cfg.kflip ? -1.0f : 1.0f);   // swap works with or without edges
  float h = keyHalf(), t = constrain((a - keyMid()) * dir, -h, h);
  return cfg.kroot + (int)roundf((t + h) / cfg.kdeg);
}

void sendMidi(Kind k, float vel) {
  uint8_t v = (uint8_t)constrain(60 + vel * 67, 1, 127);
  if (k == SHAKE) {
    uint8_t sv = (uint8_t)constrain(8 + vel * 119, 1, 127), note = shakeNote[cfg.shk];
    if (bleConnected) { MIDI.sendNoteOn(note, sv, 10); MIDI.sendNoteOff(note, 0, 10); }
#ifdef HAVE_USB_MIDI
    usbMidi.noteOn(note, sv, 10); usbMidi.noteOff(note, 0, 10);
#endif
    return;
  }
  if (bleConnected) { MIDI.sendNoteOn(midiNote[k], v, 10); MIDI.sendNoteOff(midiNote[k], 0, 10); }
#ifdef HAVE_USB_MIDI
  usbMidi.noteOn(midiNote[k], v, 10);
  usbMidi.noteOff(midiNote[k], 0, 10);
#endif
}
int lastHitKind = -1; uint32_t lastHitAt = 0;   // for the screen's colour flash
void hit(Kind k, float vel, float g = 0, float sw = -1, float lift = -1) {
  if (k != HAT) { lastKind = kindName[k]; lastHitKind = k; lastHitAt = millis(); }
  playBoard(k, vel);
  sendMidi(k, vel);
  if (ws.count()) {
    const char* nm = k == SHAKE ? (cfg.shk == 2 ? "tamb" : "shaker") : kindName[k];
    char buf[200]; int n = snprintf(buf, sizeof buf, "{\"hit\":\"%s\",\"v\":%.2f,\"g\":%.2f", nm, vel, g);
    if (sw >= 0) n += snprintf(buf + n, sizeof buf - n, ",\"sw\":%.1f", sw);
    if (lift >= 0) n += snprintf(buf + n, sizeof buf - n, ",\"lift\":%.0f", lift);
    if (hitAngle > -900 && k != HAT) { n += snprintf(buf + n, sizeof buf - n, ",\"ang\":%.0f", hitAngle); hitAngle = -999; }
    if (hitToeUp >= 0 && k != HAT && k != OHAT) { n += snprintf(buf + n, sizeof buf - n, ",\"tu\":%.0f,\"hu\":%.0f", hitToeUp, hitHeelUp); hitToeUp = hitHeelUp = -1; }
    if (hitVotes[0] && k != HAT && k != OHAT) { n += snprintf(buf + n, sizeof buf - n, ",\"votes\":\"%s\"", hitVotes); hitVotes[0] = 0; }
    snprintf(buf + n, sizeof buf - n, "}");
    ws.textAll(buf);
  }
}

float toeAngle = 0; bool hatIsOpen = false;   // degrees the foot is tilted away from flat; open-hat state
float yawDeg = 0;                                 // foot keyboard: swivel left/right from centre (deg)
float bwx = 0, bwy = 0, bwz = 0;                  // gyro bias, re-measured whenever the foot is still
float flatTilt = 0;                               // degrees away from flat in any direction
float qx = 0, qy = 0, qz = 0; bool qInit = false;   // gravity direction tracked with the gyro (instant), corrected slowly by the accelerometer
#include "pb_clock.h"

// ---------------- Motion detection ----------------
// Board taped to the top of the shoe.
//  Kick  = a stomp: an impact spike in the accelerometer above cfg.thr.
//  Snare = a swing: the shoe rotating along the learned swing direction (gyro) faster than cfg.swthr.
//          Only that direction (and sign) counts, so the toe lift before a stomp and the swing back don't trigger it.
float gx = 0, gy = 0, gz = 0;        // gravity estimate (g units)
float peak = 0; uint32_t peakT = 0, lastStompT = 0;
uint32_t lastSwingT = 0; bool inSwing = false; float swingPeak = 0;
float swingAngle = 0; uint32_t swingStill = 0;   // degrees swung (net) along the learned direction since the foot was last at rest
float meterBuf[12]; int nMeter = 0; uint32_t lastMeterSend = 0;
uint32_t stillSince = 0;                      // for capturing "flat" automatically the first time
bool upLearn = false; uint32_t upLearnEnd = 0; float upBest = 0, upBx = 0, upBy = 0, upBz = 0;
void setFlat() {
  if (!qInit) return;
  cfg.fx = qx; cfg.fy = qy; cfg.fz = qz; cfg.flatSet = true; saveCfg();
  // now raise your toe: the biggest tilt in the next 5 s tells us which way is toe-up
  upLearn = true; upLearnEnd = millis() + 5000; upBest = 0;
  ws.textAll("{\"flat\":1,\"learn\":\"now raise your toe\"}");
}
void finishUpLearn() {
  upLearn = false;
  if (upBest < 10) { ws.textAll("{\"flat\":3}"); return; }   // toe never came up far enough
  // tilt direction = the part of the raised-toe gravity reading that isn't along "flat"
  float d = upBx * cfg.fx + upBy * cfg.fy + upBz * cfg.fz;
  float tx = upBx - d * cfg.fx, ty = upBy - d * cfg.fy, tz = upBz - d * cfg.fz, tm = sqrtf(tx * tx + ty * ty + tz * tz);
  if (tm < 1e-3f) { ws.textAll("{\"flat\":3}"); return; }
  cfg.ux = tx / tm; cfg.uy = ty / tm; cfg.uz = tz / tm; cfg.upSet = true; saveCfg();
  ws.textAll("{\"flat\":2}");
}
// Signed toe angle: + toe up, - toe down, sideways roll ignored. Before the toe-up direction is
// learned it falls back to the unsigned tilt from flat.
float pitchNow() {
  if (!cfg.flatSet) return 0;
  float c = qx * cfg.fx + qy * cfg.fy + qz * cfg.fz;
  if (!cfg.upSet) return acosf(constrain(c, -1.0f, 1.0f)) * 57.2958f;
  float sgn = qx * cfg.ux + qy * cfg.uy + qz * cfg.uz;
  return atan2f(sgn, c) * 57.2958f;
}
// gyro history, to read how the foot was rotating just before an impact
const int GH = 512; float gH[GH][3]; uint32_t gHT[GH]; int gHi = 0;
float pH[GH];                              // signed toe angle at each of those samples
float qH[GH][3];                           // gravity direction at each of those samples
float yH[GH];                              // swivel (foot keyboard) at each of those samples
float upAcc[3]; int upN = 0;               // toe-up direction gathered from Set toe stomps
uint32_t onsetT = 0;                       // when the current impact started
float pkx = 0, pky = 0, pkz = 0;           // jolt direction at the impact's peak (linear accel)
uint32_t peakAt = 0, peakAtFired = 0;      // when the peak happened (for sharpness)
float onsetAngle = 0, onsetAngleFired = 0; // foot angle when the impact started
float fpkx = 0, fpky = 0, fpkz = 0;        // peak direction of the impact being classified
int learnTH = 0, learnN = 0; float learnSum[3];   // learning toe (1) / heel (2)

// How far the foot rotated along direction d (unit vector) in the 450 ms before the impact,
// counting only rotation that way: for a toe stomp, how many degrees the toe dropped.
static float dropAngle(uint32_t t0, const float* d) {
  float sum = 0;
  for (int i = 0; i < GH; i++) {
    int32_t age = (int32_t)(t0 - gHT[i]);
    if (age < 0 || age > 450) continue;
    int j = (i + GH - 1) % GH; int32_t dtm = (int32_t)(gHT[i] - gHT[j]);
    if (dtm <= 0 || dtm > 50) continue;
    float proj = gH[i][0] * d[0] + gH[i][1] * d[1] + gH[i][2] * d[2];
    if (proj > 0) sum += proj * dtm / 1000.0f;
  }
  return sum;
}
// How far the toe rose and how far the heel rose in the 400 ms before an impact, measured from where
// the foot was at the start of that window. Only the lift counts; coming back down is ignored.
static void preLift(uint32_t t0, float* toeUp, float* heelUp) {
  float start = 0, hi = -999, lo = 999; uint32_t oldest = 0xFFFFFFFF;
  for (int i = 0; i < GH; i++) {
    int32_t age = (int32_t)(t0 - gHT[i]);
    if (age < 0 || age > 400) continue;
    if (gHT[i] < oldest) { oldest = gHT[i]; start = pH[i]; }
    hi = max(hi, pH[i]); lo = min(lo, pH[i]);
  }
  if (hi < -900) { *toeUp = *heelUp = 0; return; }
  // the reference is whichever is lower of "where it started" and "flat", so a foot that was already tipped still counts
  *toeUp = max(0.0f, hi - min(start, 0.0f));
  *heelUp = max(0.0f, max(start, 0.0f) - lo);
}
static float prePitch(uint32_t t0) {   // + toe lift / - heel lift, whichever was bigger
  float tu, hu; preLift(t0, &tu, &hu);
  return tu >= hu ? tu : -hu;
}
// Average rotation (deg/s, 3-D) over [t0+from, t0+to] ms.
static void avgRotation(uint32_t t0, int from, int to, float* out) {
  out[0] = out[1] = out[2] = 0; int n = 0;
  for (int i = 0; i < GH; i++) {
    int32_t rel = (int32_t)(gHT[i] - t0);
    if (rel < from || rel > to) continue;
    out[0] += gH[i][0]; out[1] += gH[i][1]; out[2] += gH[i][2]; n++;
  }
  if (n) { out[0] /= n; out[1] /= n; out[2] /= n; }
}
static void unit3(float* v) { float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (m > 1e-3f) { v[0] /= m; v[1] /= m; v[2] /= m; } else v[0] = v[1] = v[2] = 0; }
float learnSig[11];
// The five measurements of the impact being classified:
//  rot  = which way the foot was turning just before it landed (-90..-10 ms), unit vector
//  dir  = which way the jolt pushed the board at its peak, unit vector
//  reb  = which way the foot started rocking right after the hit (+5..+35 ms), unit vector
//  rise = ms from the start of the jolt to its peak (sharp toe strike vs softer heel strike)
//  ang  = foot angle when the jolt started (degrees from flat)
static void impactSignature(float* x) {
  avgRotation(onsetT, -90, -10, x); if (sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) < 8) x[0] = x[1] = x[2] = 0; else unit3(x);
  x[3] = fpkx; x[4] = fpky; x[5] = fpkz; unit3(x + 3);
  avgRotation(onsetT, 5, 35, x + 6); if (sqrtf(x[6] * x[6] + x[7] * x[7] + x[8] * x[8]) < 8) x[6] = x[7] = x[8] = 0; else unit3(x + 6);
  x[9] = (float)(peakAtFired - onsetT);
  x[10] = cfg.flatSet ? onsetAngleFired : 0;
}
static float cos3(const float* a, const float* b) {
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  float m = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) * sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  return m > 1e-3f ? d / m : 0;
}
// Similarity of an impact to a learned strike; v[] gets each measurement's part of the score.
static float sigScore(const float* x, const float* m, float* v) {
  v[0] = cos3(x, m);
  v[1] = cos3(x + 3, m + 3);
  v[2] = 0.7f * cos3(x + 6, m + 6);
  v[3] = -0.5f * min(2.0f, fabsf(x[9] - m[9]) / max(4.0f, 0.5f * m[9]));
  v[4] = cfg.flatSet ? -0.5f * min(2.0f, fabsf(x[10] - m[10]) / 8.0f) : 0;
  return v[0] + v[1] + v[2] + v[3] + v[4];
}
// Average rotation (deg/s, 3-D) from 90 ms to 10 ms before the impact started.
static void preImpactRotation(uint32_t t0, float* out) {
  out[0] = out[1] = out[2] = 0; int n = 0;
  for (int i = 0; i < GH; i++) {
    int32_t age = (int32_t)(t0 - gHT[i]);
    if (age < 10 || age > 90) continue;
    out[0] += gH[i][0]; out[1] += gH[i][1]; out[2] += gH[i][2]; n++;
  }
  if (n) { out[0] /= n; out[1] /= n; out[2] /= n; }
}
uint32_t lastT = 0;
// swing calibration: capture up to 3 swings, average their direction
bool calibrating = false; uint32_t calEnd = 0; int calCount = 0; bool calInSwing = false;
int calState = 0; uint32_t calStillSince = 0; float calDx = 0, calDy = 0, calDz = 0;
float calBest = 0, calBx = 0, calBy = 0, calBz = 0, calSx = 0, calSy = 0, calSz = 0;

void startSwingCal() {
  calibrating = true; calEnd = millis() + 10000; calCount = 0; calInSwing = false; calState = 0;
  calSx = calSy = calSz = 0; ws.textAll("{\"learn\":\"swing your foot 0/3\"}");
}
void finishSwingCal() {
  calibrating = false;
  float m = sqrtf(calSx * calSx + calSy * calSy + calSz * calSz);
  if (calCount >= 1 && m > 0) {
    cfg.rx = calSx / m; cfg.ry = calSy / m; cfg.rz = calSz / m; cfg.cal = true; saveCfg();
    ws.textAll("{\"cal\":1}"); sendCfg(nullptr);
  } else ws.textAll("{\"cal\":0}");
}

void fireStomp(float p, float now) {
  if (cfg.snare == 10) {   // Foot keyboard: a tap plays the note the foot is swivelled to; it holds until the foot leaves flat
    // read the swivel from just before the tap: the tap's own jolt makes the gyro twitch
    float a = yawDeg; int32_t bestAge = 1 << 30; int bi = -1;
    for (int i = 0; i < GH; i++) { int32_t age = (int32_t)(onsetT - gHT[i]); if (age >= 10 && age < bestAge) { bestAge = age; a = yH[i]; bi = i; } }
    keyOn(keyForTilt(a), constrain((p - cfg.thr) / (cfg.thr * 3), 0.0f, 1.0f), a);
    // "flat" for this note = how the foot sat just before the tap
    if (bi >= 0) { keyQx = qH[bi][0]; keyQy = qH[bi][1]; keyQz = qH[bi][2]; } else { keyQx = qx; keyQy = qy; keyQz = qz; }
    keyOnAt = now; keyTip = 0;
    keyLiftSince = 0;
    return;
  }
  if (cfg.snare == 11) {   // Swivel kit: centre = kick, swivelled out = snare, swivelled in = open hi-hat
    float a = yawDeg; int32_t bestAge = 1 << 30;
    for (int i = 0; i < GH; i++) { int32_t age = (int32_t)(onsetT - gHT[i]); if (age >= 10 && age < bestAge) { bestAge = age; a = yH[i]; } }
    float rel = cfg.zflip ? -a : a;
    float vel = constrain(0.5f + 0.5f * ((p - cfg.thr) / (cfg.thr * 3)), 0.0f, 1.0f);
    if (rel < -cfg.zdeg) hit(OHAT, vel, p);                 // open hat: plays straight away, doesn't move the tempo
    else onHit(rel > cfg.zdeg ? SNARE : KICK, vel, p, -1, now);
    return;
  }
  if (cfg.snare == 9) {   // Shaker: every shake is a hit; strength sets the velocity (lightest = thr, full at 4x thr)
    float vel = constrain((p - cfg.thr) / (cfg.thr * 3), 0.0f, 1.0f);
    hit(SHAKE, vel, p);
    return;
  }
  if (cfg.snare == 7 || cfg.snare == 8) {   // toe vs heel from five measurements, learned with Set toe / Set heel
    float x[11]; impactSignature(x);
    if (learnTH) {
      for (int i = 0; i < 11; i++) learnSig[i] += x[i];
      learnN++;
      // Toe stomps also teach which way is toe-up: the most-tilted moment before each one is "toe raised".
      if (learnTH == 1 && cfg.flatSet) {
        if (learnN == 1) { upAcc[0] = upAcc[1] = upAcc[2] = 0; upN = 0; }
        float best = 0; int bi = -1;
        for (int i = 0; i < GH; i++) {
          int32_t age = (int32_t)(onsetT - gHT[i]);
          if (age < 0 || age > 450) continue;
          float c = qH[i][0] * cfg.fx + qH[i][1] * cfg.fy + qH[i][2] * cfg.fz, a = acosf(constrain(c, -1.0f, 1.0f)) * 57.2958f;
          if (a > best) { best = a; bi = i; }
        }
        if (bi >= 0 && best > 5) {
          float d = qH[bi][0] * cfg.fx + qH[bi][1] * cfg.fy + qH[bi][2] * cfg.fz;
          float tx = qH[bi][0] - d * cfg.fx, ty = qH[bi][1] - d * cfg.fy, tz = qH[bi][2] - d * cfg.fz, tm = sqrtf(tx * tx + ty * ty + tz * tz);
          if (tm > 1e-3f) { upAcc[0] += tx / tm; upAcc[1] += ty / tm; upAcc[2] += tz / tm; upN++; }
        }
        if (learnN >= 3 && upN >= 2) {
          float m = sqrtf(upAcc[0] * upAcc[0] + upAcc[1] * upAcc[1] + upAcc[2] * upAcc[2]);
          if (m > 0.5f) { cfg.ux = upAcc[0] / m; cfg.uy = upAcc[1] / m; cfg.uz = upAcc[2] / m; cfg.upSet = true; }
        }
      }
      float own[3] = { x[0], x[1], x[2] };
      char b[140];
      if (cfg.upSet) { float tu, hu; preLift(onsetT, &tu, &hu); snprintf(b, sizeof b, "{\"learn\":\"%s %d/3 (toe up %.0f deg, heel up %.0f deg)\"}", learnTH == 1 ? "toe" : "heel", learnN, tu, hu); }
      else snprintf(b, sizeof b, "{\"learn\":\"%s %d/3 (lift %.0f deg)\"}", learnTH == 1 ? "toe" : "heel", learnN, dropAngle(onsetT, own));
      ws.textAll(b);
      if (learnN >= 3) {
        float* dst = learnTH == 1 ? cfg.toeSig : cfg.heelSig;
        for (int i = 0; i < 11; i++) dst[i] = learnSig[i] / 3;
        if (learnTH == 1) cfg.lcal = true; else cfg.rcal = true;
        learnTH = 0; saveCfg(); sendCfg(nullptr);
      }
      return;
    }
    // Score against each learned strike; each measurement votes, the totals decide.
    float vt[5], vh[5];
    float st = cfg.lcal ? sigScore(x, cfg.toeSig, vt) : -99, sh = cfg.rcal ? sigScore(x, cfg.heelSig, vh) : -99;
    float toeUp = 0, heelUp = 0; if (cfg.upSet) preLift(onsetT, &toeUp, &heelUp);
    float pp = toeUp >= heelUp ? toeUp : -heelUp;   // + toe lifted more, - heel lifted more
    char votes[7] = "------";
    if (cfg.lcal && cfg.rcal) for (int i = 0; i < 5; i++) votes[i] = vt[i] > vh[i] + 0.05f ? 'T' : vh[i] > vt[i] + 0.05f ? 'H' : '=';
    if (cfg.upSet) votes[5] = (toeUp >= cfg.tthr && toeUp > heelUp) ? 'T' : (heelUp >= 4 && heelUp > toeUp) ? 'H' : '=';
    strcpy(hitVotes, votes);
    // a toe also has to have dropped far enough
    float tn = sqrtf(cfg.toeSig[0] * cfg.toeSig[0] + cfg.toeSig[1] * cfg.toeSig[1] + cfg.toeSig[2] * cfg.toeSig[2]);
    float td[3] = { tn > 0 ? cfg.toeSig[0] / tn : 0, tn > 0 ? cfg.toeSig[1] / tn : 0, tn > 0 ? cfg.toeSig[2] / tn : 0 };
    float ang = tn > 0 ? dropAngle(onsetT, td) : 0;
    bool toe;
    if (cfg.upSet && toeUp >= cfg.tthr && toeUp > heelUp) toe = true;   // toe lifted at least the Toe lift angle, more than the heel
    else if (cfg.upSet && heelUp >= 4 && heelUp > toeUp) toe = false;  // heel lifted more: heel
    else toe = st >= sh && ang >= cfg.tthr;               // unclear tilt: the five measurements decide
    hitAngle = cfg.upSet ? pp : ang; hitToeUp = toeUp; hitHeelUp = heelUp;
    Kind k = (toe == (cfg.snare == 7)) ? KICK : SNARE;
    if (p < (k == KICK ? cfg.klvl : cfg.slvl)) return;   // not hard enough for that drum
    onHit(k, constrain(0.6f + 0.4f * (p / ((k == KICK ? cfg.klvl : cfg.slvl) * 2.5f)), 0, 1), p, -1, now, fabsf(st - sh) * 30);
    return;
  }
  if (cfg.snare == 6) { onHit(KICK, constrain(0.6f + 0.4f * (p / (cfg.thr * 2.5f)), 0, 1), p, -1, now); return; }
  // tap/stomp modes: the stomp line decides; mode 2 = tap kick / stomp snare, mode 1 = the reverse
  bool stomp = p >= cfg.sthr;
  Kind k = (stomp ^ (cfg.snare == 1)) ? SNARE : KICK;
  float vel = stomp ? constrain(0.7f + 0.3f * (p / (cfg.sthr * 2)), 0, 1) : constrain(0.6f + 0.4f * (p / cfg.sthr), 0, 1);
  onHit(k, vel, p, -1, now);
}

void processMotion(float ax, float ay, float az, float wx, float wy, float wz, uint32_t now) {  // accel in g (incl. gravity), gyro in deg/s
  float dts = lastT ? min((uint32_t)50, now - lastT) / 1000.0f : 0;   // seconds since the previous sample
  lastT = now;
  const float k = 0.05f; gx += k * (ax - gx); gy += k * (ay - gy); gz += k * (az - gz);
  {
    // Track the gravity direction: rotate it with the gyro every sample (no lag), and pull it gently
    // toward the accelerometer when the board isn't being jolted, so it can't drift.
    float am = sqrtf(ax * ax + ay * ay + az * az);
    if (!qInit && am > 0.5f) { qx = ax / am; qy = ay / am; qz = az / am; qInit = true; }
    if (qInit) {
      float rx = wx * 0.0174533f, ry = wy * 0.0174533f, rz = wz * 0.0174533f;   // rad/s
      // a direction fixed in the world, seen from the rotating board: dq/dt = q x w
      float nx = qx + (qy * rz - qz * ry) * dts, ny = qy + (qz * rx - qx * rz) * dts, nz = qz + (qx * ry - qy * rx) * dts;
      if (am > 0.85f && am < 1.15f) { const float a = 0.02f; nx += a * (ax / am - nx); ny += a * (ay / am - ny); nz += a * (az / am - nz); }
      float nm = sqrtf(nx * nx + ny * ny + nz * nz); if (nm > 0) { qx = nx / nm; qy = ny / nm; qz = nz / nm; }
    }
    if (cfg.flatSet && qInit) {
      if (upLearn) {
        float c = qx * cfg.fx + qy * cfg.fy + qz * cfg.fz, a = acosf(constrain(c, -1.0f, 1.0f)) * 57.2958f;
        if (a > upBest) { upBest = a; upBx = qx; upBy = qy; upBz = qz; }
        if (now > upLearnEnd || (upBest > 15 && a < upBest * 0.5f)) finishUpLearn();   // done once the toe comes back down
      }
      toeAngle = pitchNow();
      flatTilt = acosf(constrain(qx * cfg.fx + qy * cfg.fy + qz * cfg.fz, -1.0f, 1.0f)) * 57.2958f;
      if (!hatIsOpen && toeAngle > cfg.othr) hatIsOpen = true;            // a few degrees of hysteresis
      else if (hatIsOpen && toeAngle < cfg.othr - 4) hatIsOpen = false;   // so it doesn't flutter at the line
    } else if (!cfg.flatSet) {
      // never set: take "flat" from the first full second the foot is still
      float lin = fabsf(am - 1.0f);
      if (lin < 0.04f) { if (!stillSince) stillSince = now; else if (now - stillSince > 1000) setFlat(); } else stillSince = 0;
    }
  }
  float lx = ax - gx, ly = ay - gy, lz = az - gz;               // linear accel, g
  float mag = sqrtf(lx * lx + ly * ly + lz * lz);
  float wm = sqrtf(wx * wx + wy * wy + wz * wz);               // rotation speed, deg/s
  // Swivel: rotation about the vertical (gravity) axis, minus the gyro's own small error, added up.
  if (qInit) {
    if (wm < 6 && fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f) < 0.03f) {   // still: whatever the gyro reads is its error
      const float b = 0.01f; bwx += b * (wx - bwx); bwy += b * (wy - bwy); bwz += b * (wz - bwz);
    }
    float yr = (wx - bwx) * qx + (wy - bwy) * qy + (wz - bwz) * qz;   // deg/s about vertical
    if (fabsf(yr) > 1.5f) yawDeg += yr * dts;   // ignore sub-noise creep
    if (cfg.snare == 11) yawDeg = constrain(yawDeg, -70.0f, 70.0f);
    else yawDeg = constrain(yawDeg, keyMid() - keyHalf() - 15, keyMid() + keyHalf() + 15);
  }
  gH[gHi][0] = wx; gH[gHi][1] = wy; gH[gHi][2] = wz; gHT[gHi] = now; pH[gHi] = toeAngle; qH[gHi][0] = qx; qH[gHi][1] = qy; qH[gHi][2] = qz; yH[gHi] = yawDeg; gHi = (gHi + 1) % GH;
  if (nMeter < 12) meterBuf[nMeter++] = mag;

  if (calibrating) {
    // A swing starts when rotation passes 120 deg/s. Lock onto the direction it starts in and keep the
    // strongest reading only while the foot keeps turning that way; once it reverses (the swing back)
    // the swing is counted and everything is ignored until the foot is still again.
    if (calState == 0 && wm > 120) {
      calState = 1; calBest = wm; calBx = wx; calBy = wy; calBz = wz;
      calDx = wx / wm; calDy = wy / wm; calDz = wz / wm;
    } else if (calState == 1) {
      float along = wx * calDx + wy * calDy + wz * calDz;
      if (along > calBest) { calBest = along; calBx = wx; calBy = wy; calBz = wz; }
      if (along < 50) {   // slowed down or reversed: the out-swing is over
        calCount++; calSx += calBx / calBest; calSy += calBy / calBest; calSz += calBz / calBest;
        char b[64]; snprintf(b, sizeof b, "{\"learn\":\"swing your foot %d/3\"}", calCount); ws.textAll(b);
        calState = 2; calStillSince = 0;
        if (calCount >= 3) finishSwingCal();
      }
    } else if (calState == 2) {   // ignore the swing back; wait for ~150 ms of stillness
      if (wm < 40) { if (!calStillSince) calStillSince = now; else if (now - calStillSince >= 150) calState = 0; } else calStillSince = 0;
    }
    if (calibrating && now > calEnd) finishSwingCal();
    return;
  }

  // ---- snare: swing along the learned direction (stomp/swing mode only) ----
  // Add up how far the foot has turned along the swing (swinging back subtracts). Crossing the swing angle
  // plays the snare once; it re-arms when the foot comes most of the way back, and re-zeroes when still.
  if (cfg.cal && cfg.snare == 6) {
    float proj = wx * cfg.rx + wy * cfg.ry + wz * cfg.rz;       // deg/s along the swing
    swingAngle = max(0.0f, swingAngle + proj * dts);
    if (wm < 15) { if (!swingStill) swingStill = now; else if (now - swingStill > 250) swingAngle = 0; } else swingStill = 0;
    bool clearOfStomp = now - lastStompT > 120;                  // a stomp's own jolt can spin the board; ignore that
    if (swingAngle >= cfg.swang && !inSwing && clearOfStomp && now - lastSwingT > (uint32_t)cfg.gap) {
      inSwing = true; lastSwingT = now;
      // velocity from how fast the swing was going when it crossed the line
      onHit(SNARE, constrain(0.55f + 0.45f * (proj / 400.0f), 0.0f, 1.0f), 0, swingAngle, now);
    }
    if (inSwing && swingAngle < cfg.swang * 0.4f) inSwing = false;
  }

  // ---- foot keyboard: lifting off flat releases the note ----
  if (cfg.snare == 10 && keyNote >= 0) {
    keyTip = acosf(constrain(qx * keyQx + qy * keyQy + qz * keyQz, -1.0f, 1.0f)) * 57.2958f;
    bool settled = now - keyOnAt > 250;                       // the tap's own jolt can't release the note
    if (settled && keyTip > cfg.kflat) { if (!keyLiftSince) keyLiftSince = now; else if (now - keyLiftSince > 100) keyOff(); }
    else keyLiftSince = 0;
  }

  // ---- kick: impact spike; not right after a swing (the foot landing from a swing isn't a stomp) ----
  float gate = (cfg.snare == 7 || cfg.snare == 8) ? min(cfg.klvl, cfg.slvl) : cfg.thr;
  if (mag > gate) {
    uint32_t gapMs = cfg.snare == 9 ? 55 : cfg.snare == 10 ? max((uint32_t)cfg.gap, (uint32_t)200) : (uint32_t)cfg.gap;   // shakers retrigger fast; keyboard ignores the tap's bounce
    if (now - lastStompT > gapMs && now - lastSwingT > 250) {
      if (!peakT) onsetAngle = toeAngle;
      if (mag > peak) { peak = mag; pkx = lx; pky = ly; pkz = lz; peakAt = now; if (!peakT) peakT = now; }
      if (now - peakT > (cfg.snare == 9 ? 15u : 35u)) { float p = peak; onsetT = peakT; peakAtFired = peakAt; onsetAngleFired = onsetAngle; fpkx = pkx; fpky = pky; fpkz = pkz; peak = 0; peakT = 0; lastStompT = now; fireStomp(p, now); }
    }
  } else if (peakT) { float p = peak; onsetT = peakT; peakAtFired = peakAt; onsetAngleFired = onsetAngle; fpkx = pkx; fpky = pky; fpkz = pkz; peak = 0; peakT = 0; lastStompT = now; fireStomp(p, now); }
}

// ---------------- Web ----------------
void sendCfg(AsyncWebSocketClient* c) {
  char b[880];
  snprintf(b, sizeof b, "{\"cfg\":{\"snare\":%d,\"thr\":%.3f,\"sthr\":%.3f,\"swthr\":%.0f,\"swang\":%.0f,\"kroot\":%d,\"krange\":%.0f,\"kflip\":%d,\"kflat\":%.0f,\"kspan\":%d,\"kdeg\":%.1f,\"kEdge\":%d,\"kLo\":%.1f,\"kHi\":%.1f,\"zdeg\":%.0f,\"zflip\":%d,\"tthr\":%.0f,\"shk\":%d,\"ohat\":%d,\"othr\":%.0f,\"flat\":%d,\"up\":%d,\"klvl\":%.2f,\"slvl\":%.2f,\"tlock\":%d,\"bpm\":%.0f,\"gap\":%d,\"kit\":%d,\"out\":%d,\"quant\":%d,\"hat\":%d,\"vol\":%d,\"cal\":%d,\"tcal\":%d,\"hcal\":%d,\"ver\":\"%s\",\"ssid\":\"%s\"}}",
           cfg.snare, cfg.thr, cfg.sthr, cfg.swthr, cfg.swang, cfg.kroot, cfg.krange, cfg.kflip, cfg.kflat, cfg.kspan, cfg.kdeg, cfg.kEdge ? 1 : 0, cfg.kLo, cfg.kHi, cfg.zdeg, cfg.zflip, cfg.tthr, cfg.shk, cfg.ohat, cfg.othr, cfg.flatSet ? 1 : 0, cfg.upSet ? 1 : 0, cfg.klvl, cfg.slvl, cfg.tlock, cfg.bpm, cfg.gap, cfg.kit, cfg.out, cfg.quant, cfg.hat, cfg.vol, cfg.cal ? 1 : 0, cfg.lcal ? 1 : 0, cfg.rcal ? 1 : 0, FW_VERSION, cfg.ssid);
  if (c) c->text(b); else ws.textAll(b);
}
// Tiny JSON reader for {"set":{"key":value}} / {"get":1} / {"cal":1}
float jsonNum(const char* s, const char* key, bool* found) {
  char pat[32]; snprintf(pat, sizeof pat, "\"%s\":", key);
  const char* p = strstr(s, pat); *found = p != nullptr; return p ? atof(p + strlen(pat)) : 0;
}
void onWsEvent(AsyncWebSocket* srv, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) { sendCfg(client); return; }
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo* info = (AwsFrameInfo*)arg;
  if (!(info->final && info->index == 0 && info->len == len)) return;
  char s[256]; size_t n = min(len, sizeof(s) - 1); memcpy(s, data, n); s[n] = 0;
  bool f;
  if (strstr(s, "\"get\"")) { sendCfg(client); return; }
  if (strstr(s, "\"cal\"")) { startSwingCal(); return; }
  if (strstr(s, "\"learnToe\"")) { learnTH = 1; learnN = 0; memset(learnSig, 0, sizeof learnSig); ws.textAll("{\"learn\":\"toe 0/3\"}"); return; }
  if (strstr(s, "\"learnHeel\"")) { learnTH = 2; learnN = 0; memset(learnSig, 0, sizeof learnSig); ws.textAll("{\"learn\":\"heel 0/3\"}"); return; }
  if (strstr(s, "\"setFlat\"")) { setFlat(); return; }
  if (strstr(s, "\"keyCenter\"")) { yawDeg = cfg.snare == 11 ? 0 : keyMid(); return; }
  // Edges: both must be set (since the last clear) and at least 6 degrees apart before they're used.
  if (strstr(s, "\"keyLo\"") || strstr(s, "\"keyHi\"")) {
    bool lo = strstr(s, "\"keyLo\"") != nullptr;
    if (lo) { cfg.kLo = yawDeg; loSet = true; } else { cfg.kHi = yawDeg; hiSet = true; }
    cfg.kEdge = (loSet || cfg.kEdge) && (hiSet || cfg.kEdge) && fabsf(cfg.kHi - cfg.kLo) >= 6;
    if (cfg.kEdge) cfg.kdeg = constrain(fabsf(cfg.kHi - cfg.kLo) / max(1, cfg.kspan), 1.0f, 15.0f);   // fit the notes between the edges
    saveCfg(); sendCfg(nullptr); ws.textAll(lo ? "{\"edge\":\"lo\"}" : "{\"edge\":\"hi\"}"); return;
  }
  if (strstr(s, "\"keyClear\"")) { cfg.kEdge = false; cfg.kLo = cfg.kHi = 0; loSet = hiSet = false; yawDeg = 0; saveCfg(); sendCfg(nullptr); return; }
  if (strstr(s, "\"flipSwing\"")) { cfg.rx = -cfg.rx; cfg.ry = -cfg.ry; cfg.rz = -cfg.rz; saveCfg(); ws.textAll("{\"learn\":\"flipped ✓\"}"); return; }
  if (strstr(s, "\"switchFw\"")) { otaRequest = 2; return; }
  if (strstr(s, "\"ota\"")) { otaRequest = 1; return; }
  if (const char* q = strstr(s, "\"ssid\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t n = min((size_t)(e - q), sizeof(cfg.ssid) - 1); memcpy(cfg.ssid, q, n); cfg.ssid[n] = 0; } }
  if (const char* q = strstr(s, "\"pass\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t n = min((size_t)(e - q), sizeof(cfg.pass) - 1); memcpy(cfg.pass, q, n); cfg.pass[n] = 0; } }
  float v;
  v = jsonNum(s, "thr", &f); if (f) cfg.thr = v;
  v = jsonNum(s, "sthr", &f); if (f) cfg.sthr = v;
  v = jsonNum(s, "swthr", &f); if (f) cfg.swthr = v;
  v = jsonNum(s, "swang", &f); if (f) cfg.swang = constrain(v, 3.0f, 120.0f);
  v = jsonNum(s, "kroot", &f); if (f) cfg.kroot = constrain((int)v, 24, 96);
  v = jsonNum(s, "krange", &f); if (f) cfg.krange = constrain(v, 8.0f, 80.0f);
  v = jsonNum(s, "kflip", &f); if (f) cfg.kflip = (int)v ? 1 : 0;
  v = jsonNum(s, "zdeg", &f); if (f) cfg.zdeg = constrain(v, 3.0f, 45.0f);
  v = jsonNum(s, "zflip", &f); if (f) cfg.zflip = (int)v ? 1 : 0;
  v = jsonNum(s, "kflat", &f); if (f) cfg.kflat = constrain(v, 3.0f, 40.0f);
  v = jsonNum(s, "kspan", &f); if (f) cfg.kspan = constrain((int)v, 2, 48);
  v = jsonNum(s, "kdeg", &f); if (f) cfg.kdeg = constrain(v, 1.0f, 15.0f);
  v = jsonNum(s, "tthr", &f); if (f) cfg.tthr = v;
  v = jsonNum(s, "tlock", &f); if (f) cfg.tlock = (int)v ? 1 : 0;
  v = jsonNum(s, "lthr", &f); if (f) cfg.lthr = v;
  v = jsonNum(s, "kthr", &f); if (f) cfg.kthr = v;
  v = jsonNum(s, "gap", &f); if (f) cfg.gap = (int)v;
  v = jsonNum(s, "snare", &f); if (f && (v == 1 || v == 2 || v == 6 || v == 7 || v == 8 || v == 9 || v == 10 || v == 11)) { cfg.snare = (int)v; if (v == 11) yawDeg = 0; if ((v == 9 || v == 10) && clockOn) stopClock(); keyOff(); }
  v = jsonNum(s, "shk", &f); if (f) cfg.shk = constrain((int)v, 0, 2);
  v = jsonNum(s, "ohat", &f); if (f) cfg.ohat = (int)v ? 1 : 0;
  v = jsonNum(s, "othr", &f); if (f) cfg.othr = constrain(v, 3.0f, 80.0f);
  v = jsonNum(s, "klvl", &f); if (f) cfg.klvl = v;
  v = jsonNum(s, "slvl", &f); if (f) cfg.slvl = v;
  v = jsonNum(s, "kit", &f); if (f) cfg.kit = constrain((int)v, 0, 1);
  v = jsonNum(s, "out", &f); if (f) cfg.out = (int)v;
  v = jsonNum(s, "quant", &f); if (f) cfg.quant = (int)v;
  v = jsonNum(s, "hat", &f); if (f) cfg.hat = (int)v;
  v = jsonNum(s, "vol", &f); if (f) { cfg.vol = constrain((int)v, 0, 255); M5.Speaker.setVolume(cfg.vol); }
  saveCfg();
}

// ---------------- OTA ----------------
void otaStatus(const char* msg) { char b[160]; snprintf(b, sizeof b, "{\"ota\":\"%s\"}", msg); ws.textAll(b); }
bool joinHomeWifi() {
  if (!cfg.ssid[0]) { otaStatus("No home Wi-Fi set. Enter it below first."); return false; }
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(200); M5.update(); }
  if (WiFi.status() != WL_CONNECTED) { otaStatus("Could not join home Wi-Fi (check name/password)."); return false; }
  return true;
}
// url/verUrl: which firmware to fetch. force: install even if the version matches (switching firmware).
void doOtaFromGithub(const char* url, const char* verUrl, bool force) {
  otaStatus("Joining home Wi-Fi…");
  if (!joinHomeWifi()) return;
  WiFiClientSecure client; client.setInsecure();   // GitHub over TLS without a bundled root cert
  HTTPClient http;
  otaStatus("Checking latest version…");
  http.begin(client, verUrl); int code = http.GET();
  String remote = code == 200 ? http.getString() : ""; remote.trim(); http.end();
  if (code != 200) { otaStatus("Could not reach GitHub."); return; }
  if (!force && remote == FW_VERSION) { char b[96]; snprintf(b, sizeof b, "Already up to date (%s).", FW_VERSION); otaStatus(b); return; }
  otaStatus("Downloading update…");
  http.begin(client, url); code = http.GET();
  if (code != 200) { otaStatus("Download failed."); http.end(); return; }
  int len = http.getSize();
  if (!Update.begin(len > 0 ? len : UPDATE_SIZE_UNKNOWN)) { otaStatus("Not enough space for update."); http.end(); return; }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[2048]; size_t done = 0; uint32_t lastMsg = 0;
  while (http.connected() && (len < 0 || done < (size_t)len)) {
    size_t n = stream->available(); if (!n) { delay(1); continue; }
    n = stream->readBytes(buf, min(n, sizeof buf));
    if (Update.write(buf, n) != n) { otaStatus("Write failed."); Update.abort(); http.end(); return; }
    done += n;
    if (millis() - lastMsg > 500) { char b[64]; snprintf(b, sizeof b, "Installing… %d%%", len > 0 ? (int)(done * 100 / len) : 0); otaStatus(b); lastMsg = millis(); }
  }
  http.end();
  if (Update.end(true)) { otaStatus("Update installed. Restarting…"); delay(800); ESP.restart(); }
  else otaStatus("Update failed to finish.");
}
void handleUpload(AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) { if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { otaStatus("Could not start upload."); return; } otaStatus("Receiving file…"); }
  if (Update.isRunning() && len) Update.write(data, len);
  if (final) { if (Update.end(true)) { otaStatus("Upload installed. Restarting…"); } else otaStatus("Upload failed (is it the .app.bin?)."); }
}

void setupWeb() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("PocketBeat");   // open network; the board has nothing worth protecting
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip"); r->send(res);
  });
  server.on("/samples.js", HTTP_GET, [](AsyncWebServerRequest* r) {
#ifdef PB_LITE
    r->send(200, "application/javascript", "window.PB_KITS=null;");
#else
    AsyncWebServerResponse* res = r->beginResponse(200, "application/javascript", WEB_SAMPLES_GZ, WEB_SAMPLES_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip"); res->addHeader("Cache-Control", "max-age=86400"); r->send(res);
#endif
  });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest* r) {
    bool ok = !Update.hasError(); r->send(200, "text/plain", ok ? "ok" : "fail");
    if (ok) { delay(500); ESP.restart(); }
  }, handleUpload);
  server.onNotFound([](AsyncWebServerRequest* r) { r->redirect("/"); });  // captive-portal style
  server.begin();
}

// ---------------- Display ----------------
// ---- Power: hold the side button 3 s to switch off; also switches off after 10 min with no movement and no page open.
// Press the power button to turn it back on.
uint32_t lastActive = 0;
const uint32_t AUTO_OFF_MS = 10UL * 60UL * 1000UL;
void powerDown(const char* why) {
  ws.textAll("{\"ota\":\"Board is switching off. Press its power button to turn it on.\"}");
  auto& d = M5.Display; d.wakeup(); d.setBrightness(96); d.fillScreen(TFT_BLACK); d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(2); d.setCursor(6, 20); d.print("Power off"); d.setTextSize(1); d.setCursor(6, 50); d.print(why);
  M5.Speaker.stop();
  delay(1200);
  M5.Display.setBrightness(0);
  M5.Power.powerOff();
}
void powerCheck(uint32_t now, float wx, float wy, float wz) {
  if (!lastActive) lastActive = now;
  if (ws.count() || wx * wx + wy * wy + wz * wz > 40.0f * 40.0f) lastActive = now;   // page open, or the foot is moving
  if (M5.BtnB.pressedFor(3000)) powerDown("held side button");
  if (now - lastActive > AUTO_OFF_MS) powerDown("10 min unused");
}
uint32_t lastDraw = 0, lastButtonMs = 0; bool screenOn = true;
const uint32_t SCREEN_TIMEOUT_MS = 30000; bool wakeOnly = false, resetHeld = false;
void screenSet(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  if (on) { M5.Display.wakeup(); M5.Display.setBrightness(96); }
  else { M5.Display.setBrightness(0); M5.Display.sleep(); }
}
// Each drum has its own colour on the screen: kick red, snare blue, open hat gold, shaker green.
uint16_t kindColor(int k) {
  switch (k) { case KICK: return 0xF8E4; case SNARE: return 0x2D7F; case OHAT: return TFT_GOLD; case SHAKE: return 0x87E0; case HAT: return TFT_DARKGREY; default: return TFT_WHITE; }
}
const char* kindLabel(int k) {
  switch (k) { case KICK: return "KICK"; case SNARE: return "SNARE"; case OHAT: return "OPEN HAT"; case SHAKE: return cfg.shk == 2 ? "TAMB" : "SHAKER"; case HAT: return "HAT"; default: return "TAP"; }
}
int battPct = -1; uint32_t lastSig = 0;
M5Canvas scr(&M5.Display);   // draw off-screen, then push in one go (no flicker)
void drawScreen() {
  auto& d = scr; const int W = d.width(), H = d.height(); uint32_t now = millis();
  // big word: in swivel kit, the drum a tap would play right now; otherwise the last drum played
  int zone = -1;
  if (cfg.snare == 11) { float rel = cfg.zflip ? -yawDeg : yawDeg; zone = rel < -cfg.zdeg ? OHAT : rel > cfg.zdeg ? SNARE : KICK; }
  bool flash = lastHitKind >= 0 && now - lastHitAt < 140;
  int big = flash ? lastHitKind : zone >= 0 ? zone : lastHitKind;
  int bpm = beatMs ? (int)roundf(60000.0f / beatMs) : 0;
  uint32_t sig = (big + 2) * 2654435761u ^ (flash ? 77u : 0) ^ bpm * 40503u ^ (bleConnected ? 3u : 0) ^ battPct * 1009u ^ WiFi.softAPgetStationNum() * 4099u ^ cfg.snare * 131u ^ (calibrating ? 5u : 0) ^ (cfg.cal ? 11u : 0) ^ (cfg.lcal ? 13u : 0) ^ (cfg.rcal ? 17u : 0);
  if (sig == lastSig) return; lastSig = sig;
  uint16_t col = big >= 0 ? kindColor(big) : TFT_WHITE;
  d.fillScreen(flash ? col : TFT_BLACK);
  if (!flash && big >= 0) { d.fillRect(0, 0, 6, H, col); }   // colour edge = the drum shown
  uint16_t fg = flash ? TFT_BLACK : TFT_WHITE, dim = flash ? TFT_BLACK : TFT_DARKGREY;
  d.setTextDatum(top_left); d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextColor(fg);
  char t[24]; snprintf(t, sizeof t, "%d bpm", bpm); d.drawString(t, 12, 4);
  d.setTextDatum(top_right); d.setTextColor(dim);
  snprintf(t, sizeof t, "%d%%", battPct); d.drawString(t, W - 4, 4);
  // big drum name (two-word names go on two lines)
  d.setTextDatum(middle_center); d.setFont(&fonts::FreeSansBold24pt7b); d.setTextColor(flash ? TFT_BLACK : col);
  char nm[16]; strlcpy(nm, big >= 0 ? kindLabel(big) : "--", sizeof nm);
  char* sp = strchr(nm, ' '); const char* l2 = nullptr; if (sp) { *sp = 0; l2 = sp + 1; }
  int32_t tw = max((int32_t)d.textWidth(nm), l2 ? (int32_t)d.textWidth(l2) : (int32_t)1);
  float sz = min(1.5f, (W - 16) / (float)max((int32_t)1, tw)); d.setTextSize(sz);
  int cy = 96, lh = (int)(40 * sz);
  if (l2) { d.drawString(nm, W / 2 + 3, cy - lh / 2); d.drawString(l2, W / 2 + 3, cy + lh / 2); }
  else d.drawString(nm, W / 2 + 3, cy);
  // what each gesture does / status
  d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(bottom_left); d.setTextColor(dim);
  const char* hint = calibrating ? "swing foot 3x..." : cfg.snare == 10 ? "keys: A = centre" : cfg.snare == 11 ? "in=open hat  mid=kick  out=snare" : cfg.snare == 9 ? (cfg.shk == 2 ? "tambourine" : cfg.shk == 1 ? "maracas" : "shaker") : cfg.snare == 7 ? (cfg.lcal && cfg.rcal ? "toe=kick heel=snare" : "SET TOE/HEEL") : cfg.snare == 8 ? (cfg.lcal && cfg.rcal ? "heel=kick toe=snare" : "SET TOE/HEEL") : cfg.snare == 2 ? "tap=kick stomp=snare" : cfg.snare == 1 ? "tap=snare stomp=kick" : cfg.cal ? "stomp=kick swing=snare" : "SET SWING: press A";
  if (cfg.snare == 11 && !calibrating) {   // swivel kit legend in each drum's colour
    d.setFont(&fonts::Font2); d.setTextDatum(bottom_left);
    d.setTextColor(flash ? TFT_BLACK : kindColor(OHAT)); d.drawString("in  = open hat", 12, H - 52);
    d.setTextColor(flash ? TFT_BLACK : kindColor(KICK)); d.drawString("mid = kick", 12, H - 36);
    d.setTextColor(flash ? TFT_BLACK : kindColor(SNARE)); d.drawString("out = snare", 12, H - 20);
    d.setFont(&fonts::Font0); d.setTextColor(dim);
  } else d.drawString(hint, 12, H - 14);
  snprintf(t, sizeof t, "BLE %s  WiFi %d", bleConnected ? "on" : "--", WiFi.softAPgetStationNum()); d.drawString(t, 12, H - 3);
  d.pushSprite(0, 0);
}

void setup() {
  auto c = M5.config(); c.internal_imu = true;
#ifndef PB_LITE
  c.internal_spk = true;
#endif
  M5.begin(c);
  M5.Display.setRotation(2); M5.Display.setBrightness(96);   // portrait, same as Footboard
  scr.setPsram(true); scr.setColorDepth(16); scr.createSprite(M5.Display.width(), M5.Display.height());
  lastButtonMs = millis();
  loadCfg();
  M5.Speaker.setVolume(cfg.vol);
  M5.Imu.init();
  BLEMIDI.setHandleConnected([]() { bleConnected = true; });
  BLEMIDI.setHandleDisconnected([]() { bleConnected = false; });
  MIDI.begin();
#ifdef HAVE_USB_MIDI
  USB.productName("PocketBeat"); USB.manufacturerName("PocketBeat");
  usbMidi.begin(); USB.begin();
#endif
  setupWeb();
  drawScreen();
}

void loop() {
  M5.update();
  MIDI.read();
  uint32_t now = millis();

  // Screen: off after 30 s without a button press; the next press only wakes it.
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) { lastActive = now;
    lastButtonMs = now;
    if (!screenOn) { screenSet(true); lastSig = 0; drawScreen(); wakeOnly = true; }
  }
  if (screenOn && now - lastButtonMs > SCREEN_TIMEOUT_MS) screenSet(false);
  // Button A: calibrate the "right" direction. Hold 2 s: factory reset.
  if (M5.BtnB.wasReleased() && cfg.snare == 10) {   // side button: next octave up (wraps to the bottom)
    cfg.kroot += 12; if (cfg.kroot > 84) cfg.kroot -= 48; if (cfg.kroot < 24) cfg.kroot = 24;
    keyOff(); saveCfg(); sendCfg(nullptr);
  }
  if (M5.BtnA.wasReleased()) { if (wakeOnly) wakeOnly = false; else if (!resetHeld && cfg.snare == 6) startSwingCal(); else if (!resetHeld && cfg.snare == 10) yawDeg = keyMid(); else if (!resetHeld && cfg.snare == 11) yawDeg = 0; resetHeld = false; }
  if (M5.BtnA.pressedFor(2000) && !resetHeld) {   // reset settings, keep Wi-Fi
    resetHeld = true; Cfg d; memcpy(d.ssid, cfg.ssid, sizeof d.ssid); memcpy(d.pass, cfg.pass, sizeof d.pass); d.sigVer = 7; cfg = d; saveCfg(); sendCfg(nullptr);
  }

  float ax, ay, az;
  float wx = 0, wy = 0, wz = 0;
  if (M5.Imu.update() && M5.Imu.getAccel(&ax, &ay, &az)) { M5.Imu.getGyro(&wx, &wy, &wz); processMotion(ax, ay, az, wx, wy, wz, now); }
  powerCheck(now, wx, wy, wz);

  if (otaRequest) { int r = otaRequest; otaRequest = 0; if (r == 2) doOtaFromGithub(OTHER_URL, OTHER_VER_URL, true); else doOtaFromGithub(OTA_URL, OTA_VER_URL, false); WiFi.mode(WIFI_AP); }
  if (pending.on && now >= pending.at) { pending.on = false; hit(pending.k, pending.vel, pending.g, pending.sw, pending.lift); }
  clockTick(now);

  // meter stream to the page, ~40 Hz, a dozen samples per message
  if (ws.count() && nMeter >= 12 && now - lastMeterSend > 25) {
    char b[240]; int p = snprintf(b, sizeof b, "{\"m\":[");
    for (int i = 0; i < nMeter; i++) p += snprintf(b + p, sizeof b - p, "%s%.2f", i ? "," : "", meterBuf[i]);
    snprintf(b + p, sizeof b - p, "],\"toe\":%.0f,\"open\":%d,\"yaw\":%.0f,\"flatT\":%.0f}", toeAngle, hatIsOpen ? 1 : 0, yawDeg, flatTilt); ws.textAll(b); nMeter = 0; lastMeterSend = now;
  } else if (nMeter >= 12) nMeter = 0;

  if (screenOn && now - lastDraw > 30) { lastDraw = now; drawScreen(); }   // redraws only when something changed
  static uint32_t lastHouse = 0;
  if (now - lastHouse > 2000 || battPct < 0) {
    lastHouse = now; battPct = M5.Power.getBatteryLevel();
    if (ws.count()) { char b[32]; snprintf(b, sizeof b, "{\"batt\":%d}", battPct); ws.textAll(b); }
    ws.cleanupClients();
  }
}
