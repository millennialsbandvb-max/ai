// Footboard for M5StickS3 (ESP32-S3 + BMI270)
// Invisible foot keyboard: the board lies flat on the foot. Swivel left/right to pick a note, tap to play it,
// and it holds until the foot tips away from flat. Interval buttons add harmony notes above it (stackable).
// - Sound: Bluetooth MIDI + USB MIDI (channel 1), the board speaker, and/or the phone via the tuning page.
// - Tuning page: join Wi-Fi "Footboard" and open http://192.168.4.1
// - Updates: "Check for update" on the page pulls firmware/bin/footboard_sticks3.app.bin from GitHub.
//
// Libraries: M5Unified, ESP Async WebServer (ESP32Async), Async TCP (ESP32Async), BLE-MIDI (lathoub),
//            NimBLE-Arduino 1.4.x, MIDI Library
// Board: ESP32S3 Dev Module, Flash 8MB, PSRAM: QSPI, USB CDC on boot: enabled, partition: 8MB with spiffs

#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
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
#include "fb_web.h"
#include "version.h"

static const char* OTA_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/footboard_sticks3.app.bin";
static const char* OTA_VER_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/footboard_version.txt";
static const char* OTHER_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/pocketbeat_sticks3.app.bin";   // "Switch to PocketBeat drums"
static const char* OTHER_VER_URL = "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/version.txt";

// ---------------- Settings (persisted; append new fields at the end) ----------------
struct Cfg {
  int ver = 1;
  float thr = 1.2f;        // lightest tap that plays (g of impact)
  int gap = 200;           // ms before another tap can play
  int kroot = 60;          // centre note: plays with the foot straight ahead (MIDI, 60 = C4); also the key
  int kspan = 18;          // (worked out) number of note steps across the range
  float kdeg = 4.0f;       // degrees of swivel per note
  int kflip = 1;           // (unused: the mount is fixed, higher notes are always to the right)
  float kflat = 10.0f;     // tip away from where you tapped by more than this (deg) = release
  bool kEdge = false; float kLo = 0, kHi = 0;   // swivel positions of the lowest and highest note, when set
  int ints = 0;            // harmony intervals above the note: bit 0 m3, 1 M3, 2 4th, 3 5th, 4 octave
  int out = 0;             // 0 board speaker, 1 phone, 2 both
  int vol = 200;           // board speaker volume 0-255
  int chan = 1;            // MIDI channel 1-16
  char ssid[33] = ""; char pass[65] = "";
  int scale = 0;           // 0 chromatic, 1 major, 2 minor, 3 major pentatonic, 4 minor pentatonic (key = centre note)
  int move = 0;            // how notes are picked: 0 swivel (angle), 1 slide (move the foot left/right)
  float sthr = 0.12f;      // slide: sideways push (g) that counts as a slide
  float scm = 5.0f;        // slide: cm of movement per note (0 = one note per slide)
  float rx = 1, ry = 0, rz = 0; bool rSet = false;   // slide: board direction that points to the foot's right (learned)
  float ktot = 72.0f;      // total swivel (deg) from the far left to the far right
  float kpos = 0.5f;       // where straight-ahead (the centre note) sits: share of the total that is to the left
  int cver = 0;            // settings layout version (1 = centre-note range, 2 = separate left/right note width)
  float kdegR = 4.0f;      // degrees per note on the right (higher) side; kdeg is the left (lower) side
  int nudge = 1;           // 1: swivelling (or sliding) while a note holds moves it to the new note
} cfg;
Preferences prefs;
// Home Wi-Fi is also kept in a shared "wifi" store, so PocketBeat and Footboard can each use it after a switch.
void syncWifi() {
  Preferences w; w.begin("wifi", false);
  if (cfg.ssid[0]) { if (w.getString("ssid", "") != String(cfg.ssid) || w.getString("pass", "") != String(cfg.pass)) { w.putString("ssid", cfg.ssid); w.putString("pass", cfg.pass); } }
  else { String s = w.getString("ssid", ""), p = w.getString("pass", ""); strlcpy(cfg.ssid, s.c_str(), sizeof cfg.ssid); strlcpy(cfg.pass, p.c_str(), sizeof cfg.pass); }
  w.end();
}
void saveCfg() { prefs.begin("fb", false); prefs.putBytes("cfg", &cfg, sizeof(cfg)); prefs.end(); if (cfg.ssid[0]) syncWifi(); }
void loadCfg() {
  prefs.begin("fb", true);
  size_t n = prefs.getBytesLength("cfg");
  if (n > 0 && n <= sizeof(cfg)) prefs.getBytes("cfg", &cfg, n);
  prefs.end();
  cfg.chan = constrain(cfg.chan, 1, 16);
  if (cfg.cver < 1) {   // old layout: kroot was the lowest note -> make the middle of the old range the centre note
    if (n > 0) { static const int L[5] = { 12, 7, 7, 5, 5 }, S[5][12] = { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 } };
      int sc = constrain(cfg.scale, 0, 4), m = cfg.kspan / 2; cfg.kroot = cfg.kroot + 12 * (m / L[sc]) + S[sc][m % L[sc]];
      cfg.ktot = constrain(cfg.kspan * cfg.kdeg, 5.0f, 180.0f); cfg.kpos = 0.5f; }
    cfg.cver = 1;
  }
  if (cfg.cver < 2) { cfg.kdegR = cfg.kdeg; cfg.cver = 2; }   // start both sides the same
  updateSpan();
  syncWifi();
}

static const int INT_SEMIS[5] = { 3, 4, 5, 7, 12 };          // m3, M3, 4, 5, 8
static const char* INT_NAMES[5] = { "m3", "M3", "4", "5", "8" };
static const char* NOTE_NAMES[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
void noteName(int n, char* out) { sprintf(out, "%s%d", NOTE_NAMES[((n % 12) + 12) % 12], n / 12 - 1); }

volatile int otaRequest = 0;
volatile bool wifiReq = false;   // page saved new home Wi-Fi: (re)join it
volatile int screenReq = -1;   // from the page: 0 = screen off, 1 = on
BLEMIDI_CREATE_INSTANCE("Footboard", MIDI);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
bool bleConnected = false;

// ---------------- Notes ----------------
int held[6]; int nHeld = 0;              // notes sounding now (root first), MIDI numbers
int rootHeld = -1;                       // the swivelled note of the chord, -1 = nothing held
float keyQx = 0, keyQy = 0, keyQz = 1;   // foot orientation when the note started
uint32_t keyOnAt = 0, keyLiftSince = 0;
float keyTip = 0;
char lastName[8] = "--";

// Range: the centre note plays with the foot straight ahead; each note is kdeg degrees of swivel; the range runs
// ktot degrees in total, kpos of it to the left (toes in = lower notes) and the rest to the right (higher).
// Notes are counted in steps of the chosen scale, with the centre note as the key.
static const int SC_LEN[5] = { 12, 7, 7, 5, 5 };
static const int SC_STEPS[5][12] = { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 } };
float reachL() { return cfg.ktot * cfg.kpos; }
float reachR() { return cfg.ktot * (1 - cfg.kpos); }
int nLo() { return (int)floorf(reachL() / cfg.kdeg + 0.5f); }   // notes below the centre note
int nHi() { return (int)floorf(reachR() / cfg.kdegR + 0.5f); }   // notes above (right side has its own note width)
void updateSpan() { cfg.kspan = max(1, nLo() + nHi()); }
// step i = 0 (far left, lowest) .. kspan (far right, highest); step nLo() is the centre note
int stepToNote(int i) {
  int sc = constrain(cfg.scale, 0, 4), n = SC_LEN[sc], o = i - nLo();
  int q = o >= 0 ? o / n : -((-o + n - 1) / n), r = o - q * n;
  return cfg.kroot + 12 * q + SC_STEPS[sc][r];
}
// 0..1 along the range (0 = lowest note) for a swivel angle (+ = toes left)
float rangeFrac(float a) { float st = nLo() - (a > 0 ? a / cfg.kdeg : a / cfg.kdegR); return cfg.kspan ? constrain(st / cfg.kspan, 0.0f, 1.0f) : 0.5f; }   // + = toes left (lower)
int keyForTilt(float a) { return stepToNote((int)roundf(rangeFrac(a) * cfg.kspan)); }

int slideStep = 0;   // slide mode: which step of the range the foot is on

void notesOff() {
  if (rootHeld < 0) return;
  for (int i = 0; i < nHeld; i++) {
    if (bleConnected) MIDI.sendNoteOff(held[i], 0, cfg.chan);
#ifdef HAVE_USB_MIDI
    usbMidi.noteOff(held[i], 0, cfg.chan);
#endif
    M5.Speaker.stop(i);
  }
  char b[64]; snprintf(b, sizeof b, "{\"key\":%d,\"on\":0,\"tip\":%.0f}", rootHeld, keyTip); ws.textAll(b);
  rootHeld = -1; nHeld = 0;
}
float heldVel = 0.6f;   // strength of the tap that started the held note (nudged notes reuse it)
void notesOn(int root, float vel, float swivel) {
  heldVel = vel;
  if (rootHeld >= 0) { keyTip = -1; notesOff(); }
  rootHeld = root; nHeld = 0;
  held[nHeld++] = root;
  for (int i = 0; i < 5; i++) if (cfg.ints & (1 << i)) { int n = root + INT_SEMIS[i]; if (n <= 127) held[nHeld++] = n; }
  uint8_t v = (uint8_t)constrain(20 + vel * 107, 1, 127);
  for (int i = 0; i < nHeld; i++) {
    if (bleConnected) MIDI.sendNoteOn(held[i], v, cfg.chan);
#ifdef HAVE_USB_MIDI
    usbMidi.noteOn(held[i], v, cfg.chan);
#endif
  }
  if (cfg.out != 1) {
    float each = (0.3f + 0.7f * vel) / sqrtf((float)nHeld);   // keep chords from getting louder than single notes
    for (int i = 0; i < nHeld; i++) {
      M5.Speaker.setChannelVolume(i, (uint8_t)constrain(255 * each, 0, 255));
      M5.Speaker.tone(440.0f * powf(2.0f, (held[i] - 69) / 12.0f), 60000, i, true);   // sounds until released
    }
  }
  noteName(root, lastName);
  char b[200]; int p = snprintf(b, sizeof b, "{\"key\":%d,\"on\":1,\"v\":%.2f,\"name\":\"%s\",\"tilt\":%.0f,\"notes\":[", root, vel, lastName, swivel);
  for (int i = 0; i < nHeld; i++) p += snprintf(b + p, sizeof b - p, "%s%d", i ? "," : "", held[i]);
  snprintf(b + p, sizeof b - p, "]}"); ws.textAll(b);
}

// ---------------- Motion ----------------
float qx = 0, qy = 0, qz = 0; bool qInit = false;   // gravity direction, tracked with the gyro, corrected by the accelerometer
float gx = 0, gy = 0, gz = 0;                        // slow gravity estimate for the impact detector
float bwx = 0, bwy = 0, bwz = 0;                     // gyro bias, re-learned whenever the foot is still
float yawDeg = 0;                                    // swivel from centre (deg)
float flatTilt = 0;                                  // how far the foot is tipped from where the last note started
const int GH = 256; uint32_t hT[GH]; float hY[GH]; float hQ[GH][3]; int16_t hS[GH]; int hI = 0;   // recent swivel / slide step / orientation
// slide tracking: a sideways push starts a slide; speed and distance are added up until the foot is still again
int slState = 0; uint32_t slT0 = 0, slQuiet = 0, slEnd = 0; float slV = 0, slX = 0; int slDir = 0;
float sx = 0, sy = 0, sz = 1, saPeak = 0; bool sInit = false;   // slow gravity for slides; strongest sideways push since the last report
uint32_t learnUntil = 0; float lrnX = 0, lrnY = 0, lrnZ = 0; uint32_t lrnT0 = 0;
float peak = 0; uint32_t peakT = 0, lastTapT = 0, lastT = 0;
float meterBuf[12]; int nMeter = 0; uint32_t lastMeterSend = 0;

void fireTap(float p, uint32_t onset, uint32_t now) {
  // read the swivel and the foot's orientation from just before the tap: the tap's own jolt twitches the gyro
  float a = yawDeg; int st = slideStep; float q[3] = { qx, qy, qz }; int32_t best = 1 << 30;
  for (int i = 0; i < GH; i++) {
    int32_t age = (int32_t)(onset - hT[i]);
    if (age >= 10 && age < best) { best = age; a = hY[i]; st = hS[i]; q[0] = hQ[i][0]; q[1] = hQ[i][1]; q[2] = hQ[i][2]; }
  }
  slState = 0;   // a tap ends any slide in progress
  int note = cfg.move ? stepToNote(constrain(st, 0, cfg.kspan)) : keyForTilt(a);
  notesOn(note, constrain((p - cfg.thr) / (cfg.thr * 3), 0.0f, 1.0f), cfg.move ? st : a);
  keyQx = q[0]; keyQy = q[1]; keyQz = q[2]; keyOnAt = now; keyTip = 0; keyLiftSince = 0;
}

void processMotion(float ax, float ay, float az, float wx, float wy, float wz, uint32_t now) {
  float dts = lastT ? min((uint32_t)50, now - lastT) / 1000.0f : 0; lastT = now;
  float am = sqrtf(ax * ax + ay * ay + az * az);
  if (!qInit && am > 0.5f) { qx = ax / am; qy = ay / am; qz = az / am; qInit = true; gx = ax; gy = ay; gz = az; }
  if (!qInit) return;
  { // gravity direction: rotate with the gyro (dq/dt = q x w), pull gently toward the accelerometer when not jolted
    float rx = wx * 0.0174533f, ry = wy * 0.0174533f, rz = wz * 0.0174533f;
    float nx = qx + (qy * rz - qz * ry) * dts, ny = qy + (qz * rx - qx * rz) * dts, nz = qz + (qx * ry - qy * rx) * dts;
    if (am > 0.85f && am < 1.15f) { const float k = 0.02f; nx += k * (ax / am - nx); ny += k * (ay / am - ny); nz += k * (az / am - nz); }
    float nm = sqrtf(nx * nx + ny * ny + nz * nz); if (nm > 0) { qx = nx / nm; qy = ny / nm; qz = nz / nm; }
  }
  const float k = 0.05f; gx += k * (ax - gx); gy += k * (ay - gy); gz += k * (az - gz);
  float lx = ax - gx, ly = ay - gy, lz = az - gz, mag = sqrtf(lx * lx + ly * ly + lz * lz);
  float wm = sqrtf(wx * wx + wy * wy + wz * wz);
  // swivel = rotation about vertical, minus the gyro's own error, added up
  if (wm < 6 && fabsf(am - 1.0f) < 0.03f) { const float b = 0.01f; bwx += b * (wx - bwx); bwy += b * (wy - bwy); bwz += b * (wz - bwz); }
  float yr = (wx - bwx) * qx + (wy - bwy) * qy + (wz - bwz) * qz;
  if (fabsf(yr) > 1.5f) yawDeg += yr * dts;
  yawDeg = constrain(yawDeg, -reachR() - 15, reachL() + 15);
  // ---- slide: sideways movement of the whole foot ----
  {
    // Gravity for slides: turned with the gyro, but only nudged toward the accelerometer very slowly (and not at all
    // mid-slide); the fast-corrected one above soaks up a slow sideways push within a few hundredths of a second.
    if (!sInit) { sx = qx; sy = qy; sz = qz; sInit = true; }
    { float rx = (wx - bwx) * 0.0174533f, ry = (wy - bwy) * 0.0174533f, rz = (wz - bwz) * 0.0174533f;
      float nx = sx + (sy * rz - sz * ry) * dts, ny = sy + (sz * rx - sx * rz) * dts, nz = sz + (sx * ry - sy * rx) * dts;
      if (slState == 0 && !lrnT0 && am > 0.9f && am < 1.1f) { float k2 = min(1.0f, dts * 0.6f); nx += k2 * (ax / am - nx); ny += k2 * (ay / am - ny); nz += k2 * (az / am - nz); }
      float nm = sqrtf(nx * nx + ny * ny + nz * nz); if (nm > 0) { sx = nx / nm; sy = ny / nm; sz = nz / nm; } }
    float la[3] = { ax - sx, ay - sy, az - sz };                   // movement acceleration (g), gravity removed
    float lv = la[0] * sx + la[1] * sy + la[2] * sz;               // up/down part
    float hx = la[0] - lv * sx, hy = la[1] - lv * sy, hz = la[2] - lv * sz, hm = sqrtf(hx * hx + hy * hy + hz * hz);
    if (learnUntil) {   // "Learn right": the first push after the button is the foot's right
      if (now > learnUntil) { learnUntil = 0; ws.textAll("{\"learn\":\"timeout\"}"); }
      else if (!lrnT0 && hm > max(cfg.sthr, 0.1f) && fabsf(lv) < max(0.3f, hm)) { lrnT0 = now; lrnX = lrnY = lrnZ = 0; }
      if (lrnT0) { lrnX += hx; lrnY += hy; lrnZ += hz;
        if (now - lrnT0 > 60) { float m = sqrtf(lrnX * lrnX + lrnY * lrnY + lrnZ * lrnZ);
          if (m > 0) { cfg.rx = lrnX / m; cfg.ry = lrnY / m; cfg.rz = lrnZ / m; cfg.rSet = true; saveCfg(); sendCfg(nullptr); ws.textAll("{\"learn\":\"ok\"}"); }
          learnUntil = 0; lrnT0 = 0; slEnd = now; } }
    } else if (cfg.move) {
      // the learned right direction, kept level with the ground
      float d = cfg.rx * sx + cfg.ry * sy + cfg.rz * sz, rxh = cfg.rx - d * sx, ryh = cfg.ry - d * sy, rzh = cfg.rz - d * sz;
      float rm = sqrtf(rxh * rxh + ryh * ryh + rzh * rzh); if (rm > 0.01f) { rxh /= rm; ryh /= rm; rzh /= rm; }
      float sa = hx * rxh + hy * ryh + hz * rzh;                    // + = toward the foot's right (g)
      if (fabsf(sa) > fabsf(saPeak)) saPeak = sa;
      bool tapping = peakT || now - lastTapT < 200;
      if (slState == 0) {
        if (!tapping && now - slEnd > 250 && fabsf(sa) > cfg.sthr && fabsf(lv) < max(0.3f, fabsf(sa))) {
          slState = 1; slT0 = now; slV = 0; slX = 0; slDir = sa > 0 ? 1 : -1; slQuiet = 0;
        }
      }
      if (slState == 1) {
        slV += sa * 9.81f * dts; slX += slV * dts;                   // metres
        if (hm < 0.04f) { if (!slQuiet) slQuiet = now; } else slQuiet = 0;
        if ((slQuiet && now - slQuiet > 80) || now - slT0 > 800) {
          float cm = fabsf(slX) * 100;
          int steps = cfg.scm > 0 ? max(1, min(6, (int)roundf(cm / cfg.scm))) : 1;
          int dir = slDir;   // slide right = higher
          slideStep = constrain(slideStep + dir * steps, 0, cfg.kspan);
          char b[80]; snprintf(b, sizeof b, "{\"slide\":%d,\"cm\":%.0f,\"step\":%d}", slDir * steps, cm, slideStep); ws.textAll(b);
          slState = 0; slEnd = now;
        }
      }
    }
  }
  hS[hI] = slideStep;
  hT[hI] = now; hY[hI] = yawDeg; hQ[hI][0] = qx; hQ[hI][1] = qy; hQ[hI][2] = qz; hI = (hI + 1) % GH;
  if (nMeter < 12) meterBuf[nMeter++] = mag;

  // release: the foot tipped away from where it was when the note started
  if (rootHeld >= 0) {
    keyTip = acosf(constrain(qx * keyQx + qy * keyQy + qz * keyQz, -1.0f, 1.0f)) * 57.2958f;
    flatTilt = keyTip;
    bool settled = now - keyOnAt > 250;   // the tap's own jolt can't release it
    if (settled && keyTip > cfg.kflat) { if (!keyLiftSince) keyLiftSince = now; else if (now - keyLiftSince > 100) notesOff(); }
    else keyLiftSince = 0;
  } else flatTilt = 0;

  // nudge: while a note holds, moving onto another note switches to it (same strength, same release reference).
  // It has to be well inside the new note so it doesn't flicker on the border.
  if (rootHeld >= 0 && cfg.nudge && now - keyOnAt > 150) {
    int a = aimNote();
    float xs = rangeFrac(yawDeg) * cfg.kspan, cl = 1.0f - 2.0f * fabsf(xs - roundf(xs));
    if (a != rootHeld && (cfg.move || cl > 0.4f)) {
      uint32_t on = keyOnAt; float rx0 = keyQx, ry0 = keyQy, rz0 = keyQz;
      notesOn(a, heldVel, cfg.move ? slideStep : yawDeg);
      keyOnAt = on; keyQx = rx0; keyQy = ry0; keyQz = rz0;
    }
  }

  // tap: an impact spike, played 35 ms after it starts (or when it ends) at its peak strength
  if (mag > cfg.thr) {
    if (now - lastTapT > (uint32_t)max(cfg.gap, 120)) {
      if (mag > peak) { peak = mag; if (!peakT) peakT = now; }
      if (now - peakT > 35) { float p = peak; uint32_t o = peakT; peak = 0; peakT = 0; lastTapT = now; fireTap(p, o, now); }
    }
  } else if (peakT) { float p = peak; uint32_t o = peakT; peak = 0; peakT = 0; lastTapT = now; fireTap(p, o, now); }
}

int aimNote() { return cfg.move ? stepToNote(constrain(slideStep, 0, cfg.kspan)) : keyForTilt(yawDeg); }

// ---------------- Web ----------------
void notesOff();
// change scale, keeping about the same span in octaves
void setScale(int ns) {
  cfg.scale = constrain(ns, 0, 4); updateSpan();
  slideStep = constrain(slideStep, 0, cfg.kspan); keyTip = -1; notesOff();
}
void sendCfg(AsyncWebSocketClient* c) {
  char b[600];
  snprintf(b, sizeof b, "{\"cfg\":{\"thr\":%.2f,\"gap\":%d,\"kroot\":%d,\"kspan\":%d,\"nLo\":%d,\"ktot\":%.0f,\"kpos\":%.2f,\"kdeg\":%.1f,\"kdegR\":%.1f,\"nudge\":%d,\"kflip\":%d,\"kflat\":%.0f,\"kEdge\":%d,\"kLo\":%.1f,\"kHi\":%.1f,\"ints\":%d,\"out\":%d,\"vol\":%d,\"chan\":%d,\"scale\":%d,\"move\":%d,\"sthr\":%.2f,\"scm\":%.1f,\"rSet\":%d,\"ver\":\"%s\",\"ssid\":\"%s\"}}",
           cfg.thr, cfg.gap, cfg.kroot, cfg.kspan, nLo(), cfg.ktot, cfg.kpos, cfg.kdeg, cfg.kdegR, cfg.nudge, cfg.kflip, cfg.kflat, cfg.kEdge ? 1 : 0, cfg.kLo, cfg.kHi, cfg.ints, cfg.out, cfg.vol, cfg.chan, cfg.scale, cfg.move, cfg.sthr, cfg.scm, cfg.rSet ? 1 : 0, FW_VERSION, cfg.ssid);
  if (c) c->text(b); else ws.textAll(b);
}
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
  if (strstr(s, "\"keyCenter\"")) { yawDeg = 0; slideStep = nLo(); return; }
  if (strstr(s, "\"learnRight\"")) { learnUntil = millis() + 3000; lrnT0 = 0; return; }
  if (strstr(s, "\"allOff\"")) { keyTip = -1; notesOff(); return; }
  if (strstr(s, "\"switchFw\"")) { otaRequest = 2; return; }
  if (const char* q = strstr(s, "\"screen\":")) { screenReq = atoi(q + 9) ? 1 : 0; return; }
  if (strstr(s, "\"ota\"")) { otaRequest = 1; return; }
  if (const char* q = strstr(s, "\"ssid\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t m = min((size_t)(e - q), sizeof(cfg.ssid) - 1); memcpy(cfg.ssid, q, m); cfg.ssid[m] = 0; } }
  if (const char* q = strstr(s, "\"pass\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t m = min((size_t)(e - q), sizeof(cfg.pass) - 1); memcpy(cfg.pass, q, m); cfg.pass[m] = 0; } }
  if (strstr(s, "\"ssid\":\"")) wifiReq = true;
  float v;
  v = jsonNum(s, "thr", &f); if (f) cfg.thr = constrain(v, 0.05f, 8.0f);
  v = jsonNum(s, "gap", &f); if (f) cfg.gap = constrain((int)v, 60, 600);
  v = jsonNum(s, "kroot", &f); if (f) { cfg.kroot = constrain((int)v, 24, 96); keyTip = -1; notesOff(); }
  v = jsonNum(s, "kdeg", &f); if (f) cfg.kdeg = constrain(v, 1.0f, 30.0f);
  v = jsonNum(s, "kdegR", &f); if (f) cfg.kdegR = constrain(v, 1.0f, 30.0f);
  v = jsonNum(s, "nudge", &f); if (f) cfg.nudge = (int)v ? 1 : 0;
  v = jsonNum(s, "ktot", &f); if (f) cfg.ktot = constrain(v, 5.0f, 180.0f);
  v = jsonNum(s, "kpos", &f); if (f) cfg.kpos = constrain(v, 0.0f, 1.0f);
  updateSpan();
  v = jsonNum(s, "kflip", &f); if (f) cfg.kflip = (int)v ? 1 : 0;
  v = jsonNum(s, "kflat", &f); if (f) cfg.kflat = constrain(v, 3.0f, 40.0f);
  v = jsonNum(s, "ints", &f); if (f) cfg.ints = constrain((int)v, 0, 31);
  v = jsonNum(s, "out", &f); if (f) { cfg.out = constrain((int)v, 0, 2); if (cfg.out == 1) for (int i = 0; i < 6; i++) M5.Speaker.stop(i); }
  v = jsonNum(s, "vol", &f); if (f) { cfg.vol = constrain((int)v, 0, 255); M5.Speaker.setVolume(cfg.vol); }
  v = jsonNum(s, "scale", &f);
  if (f) {   // keep about the same span in octaves when the scale changes
    int ns = constrain((int)v, 0, 4);
    if (ns != cfg.scale) setScale(ns);
  }
  v = jsonNum(s, "move", &f); if (f) { cfg.move = (int)v ? 1 : 0; slideStep = nLo(); slState = 0; }
  v = jsonNum(s, "sthr", &f); if (f) cfg.sthr = constrain(v, 0.03f, 1.0f);
  v = jsonNum(s, "scm", &f); if (f) cfg.scm = constrain(v, 0.0f, 30.0f);
  slideStep = constrain(slideStep, 0, cfg.kspan);
  v = jsonNum(s, "chan", &f); if (f) { keyTip = -1; notesOff(); cfg.chan = constrain((int)v, 1, 16); }
  saveCfg();
  sendCfg(nullptr);
}

// ---------------- OTA ----------------
void otaStatus(const char* msg) { char b[160]; snprintf(b, sizeof b, "{\"ota\":\"%s\"}", msg); ws.textAll(b); }
bool joinHomeWifi() {
  if (!cfg.ssid[0]) { otaStatus("No home Wi-Fi set. Enter it below first."); return false; }
  if (WiFi.status() == WL_CONNECTED) return true;   // already on it (normal case now)
  WiFi.mode(WIFI_AP_STA); WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(200); M5.update(); }
  if (WiFi.status() != WL_CONNECTED) { otaStatus("Could not join home Wi-Fi (check name/password)."); return false; }
  return true;
}
// url/verUrl: which firmware to fetch. force: install even if the version matches (switching firmware).
void doOtaFromGithub(const char* url, const char* verUrl, bool force) {
  otaStatus("Joining home Wi-Fi…");
  if (!joinHomeWifi()) return;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  otaStatus("Checking latest version…");
  int code = 0; String remote;
  for (int tries = 0; tries < 3 && code != 200; tries++) {   // DNS/TLS can fail the first time right after joining
    if (tries) { otaStatus("Retrying GitHub…"); delay(1500); }
    http.begin(client, verUrl); code = http.GET();
    remote = code == 200 ? http.getString() : ""; remote.trim(); http.end();
  }
  if (code != 200) {
    char b[120]; snprintf(b, sizeof b, "Could not reach GitHub (error %d, %u KB free). Try again, or use your phone's hotspot.", code, (unsigned)(ESP.getFreeHeap() / 1024));
    otaStatus(b); return;
  }
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
  if (final) { if (Update.end(true)) otaStatus("Upload installed. Restarting…"); else otaStatus("Upload failed (is it the .app.bin?)."); }
}
void setupWeb() {
  // Own network "Footboard" is always there; the board also joins your Wi-Fi (or phone hotspot) when one is saved,
  // so the phone can stay on it and open http://footboard.local
  WiFi.mode(WIFI_AP_STA); WiFi.setAutoReconnect(true);
  WiFi.softAP("Footboard");
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", FB_INDEX_GZ, FB_INDEX_GZ_LEN);
    res->addHeader("Content-Encoding", "gzip"); r->send(res);
  });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest* r) {
    bool ok = !Update.hasError(); r->send(200, "text/plain", ok ? "ok" : "fail");
    if (ok) { delay(500); ESP.restart(); }
  }, handleUpload);
  server.onNotFound([](AsyncWebServerRequest* r) { r->redirect("/"); });
  server.begin();
  if (cfg.ssid[0]) WiFi.begin(cfg.ssid, cfg.pass);
}

// ---------------- Screen ----------------
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
uint32_t lastDraw = 0, lastButtonMs = 0; bool screenOn = true, wakeOnly = false, resetHeld = false;
const uint32_t SCREEN_TIMEOUT_MS = 30000;
void screenSet(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  if (on) { M5.Display.wakeup(); M5.Display.setBrightness(96); } else { M5.Display.setBrightness(0); M5.Display.sleep(); }
}
int battPct = -1;
uint32_t lastSig = 0;   // what the screen last showed; 0 forces a redraw
M5Canvas scr(&M5.Display);
// The screen buffer is released during updates so the secure (TLS) connection has enough memory.
void screenBufFree() { scr.deleteSprite(); }
void screenBufRestore() { scr.setPsram(true); scr.setColorDepth(16); scr.createSprite(M5.Display.width(), M5.Display.height()); lastSig = 0; }
// where the cursor sits along the range, 0 = lowest note .. 1 = highest (same as the page's strip)
float cursorFrac() {
  if (cfg.move) return cfg.kspan ? constrain((float)slideStep / cfg.kspan, 0.0f, 1.0f) : 0.5f;
  return rangeFrac(yawDeg);
}
// Colour: each pitch has a fixed colour (see PITCH_RGB). The whole screen
// takes the colour of the note you're on: dim between notes, vivid on the note's centre, full while it plays.
// Naturals are the rainbow, ROYGBIV: C red, D orange, E yellow, F green, G blue, A indigo, B violet.
// Sharps/flats follow one rule: the two naturals they sit between, mixed and lightened 45% toward white (pastel),
// crossed with white stripes (C# = peach, D# = cream, F# = soft teal, G# = periwinkle, A# = lilac).
static const uint32_t PITCH_RGB[12] = {
  0xE8202A,  // C  red
  0xF9A27E,  // C# pastel of C + D, white stripes
  0xFF8C00,  // D  orange
  0xFFD77A,  // D# pastel of D + E, white stripes
  0xFFE21A,  // E  yellow
  0x1DB233,  // F  green
  0x83BDC7,  // F# pastel of F + G, white stripes
  0x1F5BFF,  // G  blue
  0x9498EB,  // G# pastel of G + A, white stripes
  0x5A2DB5,  // A  indigo
  0xBD94E4,  // A# pastel of A + B, white stripes
  0xB44CE6,  // B  violet
};
uint16_t pitchColor(int note, float bright) {   // bright 0..1
  uint32_t c = PITCH_RGB[((note % 12) + 12) % 12]; bright = constrain(bright, 0.0f, 1.0f);
  return M5.Display.color565((uint8_t)(((c >> 16) & 255) * bright), (uint8_t)(((c >> 8) & 255) * bright), (uint8_t)((c & 255) * bright));
}
bool isSharp(int note) { int pc = ((note % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
// fill an area in a note's colour: naturals solid; sharps/flats = their pastel colour crossed with white 45-degree stripes (s = colour band width, white bands are 0.6 s)
void fillPitch(M5Canvas& d, int x, int y, int w, int h, int note, int s) {
  d.fillRect(x, y, w, h, pitchColor(note, 1));
  if (!isSharp(note)) return;
  int ws = max(1, s * 3 / 5);
  d.setClipRect(x, y, w, h);
  for (int k = -h; k < w; k += s + ws) {   // white band between the lines through (x+k, y+h)->(x+k+h, y) and ws further right
    d.fillTriangle(x + k, y + h, x + k + ws, y + h, x + k + ws + h, y, TFT_WHITE);
    d.fillTriangle(x + k, y + h, x + k + ws + h, y, x + k + h, y, TFT_WHITE);
  }
  d.clearClipRect();
}
// white text with a black border
void drawOutlined(M5Canvas& d, const char* t, int x, int y, int b, uint16_t fill) {
  d.setTextColor(TFT_BLACK);
  static const int8_t DX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, DY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };   // 8 directions keeps it quick
  for (int r = b; r > 0; r = (r >= 3 ? r / 2 : 0)) for (int i = 0; i < 8; i++) d.drawString(t, x + DX[i] * r, y + DY[i] * r);
  d.setTextColor(fill); d.drawString(t, x, y);
}
void drawScreen() {
  auto& d = scr; const int W = d.width(), H = d.height();
  static const char* SCN[5] = { "Chromatic", "Major", "Minor", "Major pent", "Minor pent" };
  int shown = rootHeld >= 0 ? rootHeld : aimNote();
  float xs = cursorFrac() * cfg.kspan;                               // continuous position in steps
  float close = cfg.move ? 1.0f : constrain(1.0f - 2.0f * fabsf(xs - roundf(xs)), 0.0f, 1.0f);   // 1 = dead centre of the note
  int cx = (int)(xs * 40);   // tuner needle position (for redraw detection)
  // only redraw when something visible changed, so the motion loop isn't held up
  uint32_t sig = shown * 2654435761u ^ (rootHeld + 1) * 40503u ^ cx * 97u ^ (int)(close * 10) * 50021u ^ cfg.ints * 7919u ^ cfg.scale * 131u ^ cfg.kroot * 31337u ^ cfg.kspan * 613u ^ (bleConnected ? 1 : 0) * 2u ^ battPct * 1009u ^ (WiFi.status() == WL_CONNECTED ? 4099u : 0u);
  if (sig == lastSig) return; lastSig = sig;
  // whole screen in the note's colour
  fillPitch(d, 0, 0, W, H, shown, 18);   // whole screen in the note colour (sharps striped)
  // scale + key
  d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextDatum(top_center);
  char key[8]; noteName(cfg.kroot, key); key[strcspn(key, "0123456789-")] = 0;
  char top[32]; snprintf(top, sizeof top, "%s %s", key, SCN[constrain(cfg.scale, 0, 4)]); drawOutlined(d, top, W / 2, 3, 1, TFT_WHITE);
  // big note: white letters, black border
  d.setTextDatum(middle_center); d.setFont(&fonts::FreeSansBold24pt7b);
  const char* pc = NOTE_NAMES[((shown % 12) + 12) % 12];
  float sz = min(2.2f, (W - 12) / (float)max((int32_t)1, (int32_t)d.textWidth(pc)));
  uint16_t letter = rootHeld >= 0 ? M5.Display.color565(255, 90, 80) : TFT_WHITE;   // reddish while the note sounds
  d.setTextSize(sz); drawOutlined(d, pc, W / 2, 64, 4, letter);
  d.setFont(&fonts::FreeSansBold18pt7b); d.setTextSize(1);
  char oc[6]; snprintf(oc, sizeof oc, "%d", shown / 12 - 1); drawOutlined(d, oc, W / 2, 124, 3, letter);
  // harmony
  d.setFont(&fonts::Font2); d.setTextDatum(top_center);
  char hb[24] = ""; for (int i = 0; i < 5; i++) if (cfg.ints & (1 << i)) { strcat(hb, "+"); strcat(hb, INT_NAMES[i]); strcat(hb, " "); }
  drawOutlined(d, hb[0] ? hb : "single notes", W / 2, 136, 1, TFT_WHITE);
  // tuner on a black band: where you are between the previous and next note; centre = dead on the note
  int ty = 154; d.fillRect(0, ty, W, 72, TFT_BLACK);
  // whole range: every note as a solid block in its colour, white marker = where the foot points
  { int n = cfg.kspan + 1, rx = 3, rw = W - 6, ry = ty + 5, rh = 14;
    for (int i = 0; i < n; i++) {
      int note = stepToNote(i), xa = rx + i * rw / n, xb = rx + (i + 1) * rw / n - 1;
      fillPitch(d, xa, ry, max(1, xb - xa), rh, note, 3);   // sharps striped
      for (int q = 0; q < nHeld; q++) if (held[q] == note) d.drawRect(xa, ry, max(1, xb - xa), rh, TFT_WHITE);
    }
    int mk = rx + (int)((xs + 0.5f) / n * rw);
    d.fillRect(mk - 2, ry - 4, 5, rh + 8, TFT_BLACK); d.fillRect(mk - 1, ry - 3, 3, rh + 6, TFT_WHITE); }
  int aimStep = constrain((int)roundf(xs), 0, cfg.kspan);
  float off = cfg.move ? 0.0f : constrain(xs - aimStep, -0.5f, 0.5f);   // -0.5 = halfway to the note below, +0.5 = halfway to the one above
  int mx = W / 2, half = W / 2 - 22, my = ty + 44;
  // neighbours, each in its own colour
  d.setFont(&fonts::Font2); d.setTextSize(1); d.setTextDatum(middle_left);
  char nb[8];
  if (aimStep > 0) { noteName(stepToNote(aimStep - 1), nb); d.setTextColor(pitchColor(stepToNote(aimStep - 1), 1)); } else { strcpy(nb, "--"); d.setTextColor(TFT_DARKGREY); }
  d.drawString(nb, 2, my); d.setTextDatum(middle_right);
  if (aimStep < cfg.kspan) { noteName(stepToNote(aimStep + 1), nb); d.setTextColor(pitchColor(stepToNote(aimStep + 1), 1)); } else { strcpy(nb, "--"); d.setTextColor(TFT_DARKGREY); }
  d.drawString(nb, W - 2, my);
  // scale: ticks every 1/10 of a note, centre zone glows in the note's colour when you're close
  uint16_t nc = pitchColor(shown, 1);
  d.fillRect(mx - half, my - 1, half * 2, 3, 0x4208);
  if (close > 0.8f || rootHeld >= 0) fillPitch(d, mx - half / 5, my - 8, half * 2 / 5, 17, shown, 5); else d.fillRect(mx - half / 5, my - 8, half * 2 / 5, 17, 0x2104);
  for (int t = -5; t <= 5; t++) { int x = mx + t * half / 5; int hgt = t == 0 ? 12 : (t % 5 == 0 ? 10 : 5); d.fillRect(x, my - hgt, 1, hgt * 2 + 1, t == 0 ? TFT_WHITE : TFT_LIGHTGREY); }
  int nx = mx + (int)(off * 2 * half);
  d.fillTriangle(nx - 6, my - 18, nx + 6, my - 18, nx, my - 6, TFT_WHITE);
  d.fillRect(nx - 1, my - 18, 3, 36, TFT_WHITE);
  // status
  d.fillRect(0, H - 13, W, 13, TFT_BLACK);
  d.setTextDatum(bottom_center); d.setTextColor(TFT_LIGHTGREY);
  char st[40]; snprintf(st, sizeof st, "BLE %s  WiFi %s  %d%%", bleConnected ? "on" : "--", WiFi.status() == WL_CONNECTED ? "ok" : "--", battPct);
  d.drawString(st, W / 2, H - 2);
  d.pushSprite(0, 0);
}

void setup() {
  auto c = M5.config(); c.internal_imu = true; c.internal_spk = true;
  M5.begin(c);
  M5.Display.setRotation(2); M5.Display.setBrightness(96);   // portrait
  scr.setPsram(true); scr.setColorDepth(16); scr.createSprite(M5.Display.width(), M5.Display.height());
  lastButtonMs = millis();
  loadCfg();
  M5.Speaker.setVolume(cfg.vol);
  M5.Imu.init();
  BLEMIDI.setHandleConnected([]() { bleConnected = true; });
  BLEMIDI.setHandleDisconnected([]() { bleConnected = false; });
  MIDI.begin();
#ifdef HAVE_USB_MIDI
  USB.productName("Footboard"); USB.manufacturerName("Footboard");
  usbMidi.begin(); USB.begin();
#endif
  setupWeb();
  drawScreen();
}

void loop() {
  M5.update();
  MIDI.read();
  uint32_t now = millis();
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) { lastActive = now;
    lastButtonMs = now;
    if (!screenOn) { screenSet(true); lastSig = 0; drawScreen(); wakeOnly = true; }
  }
  // Screen stays on. Hold the front button 1 s to turn it off; any button turns it back on.
  if (M5.BtnA.pressedFor(1000) && !resetHeld && screenOn && !wakeOnly) { resetHeld = true; screenSet(false); }
  if (screenReq >= 0) { bool on = screenReq; screenReq = -1; screenSet(on); if (on) { lastSig = 0; drawScreen(); } }
  if (M5.BtnB.wasReleased() && !wakeOnly) {   // side button: next scale (chromatic > major > minor > major pent > minor pent)
    setScale((cfg.scale + 1) % 5); saveCfg(); sendCfg(nullptr); lastDraw = 0;
  }
  if (M5.BtnA.wasReleased()) { if (!wakeOnly && !resetHeld) { yawDeg = 0; slideStep = nLo(); } resetHeld = false; }
  if ((M5.BtnA.wasReleased() || M5.BtnB.wasReleased()) && wakeOnly) wakeOnly = false;
  static bool factoryDone = false;
  if (M5.BtnA.wasReleased()) factoryDone = false;
  if (M5.BtnA.pressedFor(6000) && !factoryDone) {   // hold front button 6 s: reset settings, keep Wi-Fi
    factoryDone = true; Cfg d; memcpy(d.ssid, cfg.ssid, sizeof d.ssid); memcpy(d.pass, cfg.pass, sizeof d.pass); d.cver = 2; cfg = d; updateSpan(); saveCfg(); sendCfg(nullptr);
  }

  float ax, ay, az, wx = 0, wy = 0, wz = 0;
  if (M5.Imu.update() && M5.Imu.getAccel(&ax, &ay, &az)) { M5.Imu.getGyro(&wx, &wy, &wz); processMotion(ax, ay, az, wx, wy, wz, now); }
  powerCheck(now, wx, wy, wz);

  if (wifiReq) { wifiReq = false; WiFi.disconnect(); if (cfg.ssid[0]) WiFi.begin(cfg.ssid, cfg.pass); }
  { static bool mdnsUp = false; static uint32_t lastNet = 0;
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !mdnsUp) { mdnsUp = MDNS.begin("footboard"); if (mdnsUp) MDNS.addService("http", "tcp", 80); }
    if (!up && mdnsUp) { MDNS.end(); mdnsUp = false; }
    if (ws.count() && now - lastNet > 3000) { lastNet = now; char b[120];
      snprintf(b, sizeof b, "{\"net\":\"%s\",\"host\":\"footboard.local\",\"wifi\":\"%s\"}", up ? WiFi.localIP().toString().c_str() : "", up ? cfg.ssid : ""); ws.textAll(b); } }
  if (otaRequest) { int r = otaRequest; otaRequest = 0; screenBufFree(); if (r == 2) doOtaFromGithub(OTHER_URL, OTHER_VER_URL, true); else doOtaFromGithub(OTA_URL, OTA_VER_URL, false); screenBufRestore(); }

  if (ws.count() && nMeter >= 12 && now - lastMeterSend > 25) {
    char b[240]; int p = snprintf(b, sizeof b, "{\"m\":[");
    for (int i = 0; i < nMeter; i++) p += snprintf(b + p, sizeof b - p, "%s%.2f", i ? "," : "", meterBuf[i]);
    snprintf(b + p, sizeof b - p, "],\"yaw\":%.1f,\"tip\":%.0f,\"step\":%d,\"sa\":%.2f}", yawDeg, flatTilt, slideStep, saPeak); saPeak = 0; ws.textAll(b); nMeter = 0; lastMeterSend = now;
  } else if (nMeter >= 12) nMeter = 0;

  if (screenOn && now - lastDraw > 80) { lastDraw = now; drawScreen(); }
  static uint32_t lastHouse = 0;
  if (now - lastHouse > 250) {
    lastHouse = now;
    static uint32_t lastBatt = 0;
    if (now - lastBatt > 2000 || battPct < 0) { lastBatt = now; battPct = M5.Power.getBatteryLevel(); if (ws.count()) { char b[32]; snprintf(b, sizeof b, "{\"batt\":%d}", battPct); ws.textAll(b); } }
    ws.cleanupClients();
  }
}
