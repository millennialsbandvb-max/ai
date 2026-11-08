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
  int kroot = 48;          // lowest note (MIDI, 48 = C3)
  int kspan = 18;          // semitones from lowest to highest note (18 = 1.5 octaves)
  float kdeg = 4.0f;       // degrees of swivel per note
  int kflip = 1;           // swap which way is up
  float kflat = 10.0f;     // tip away from where you tapped by more than this (deg) = release
  bool kEdge = false; float kLo = 0, kHi = 0;   // swivel positions of the lowest and highest note, when set
  int ints = 0;            // harmony intervals above the note: bit 0 m3, 1 M3, 2 4th, 3 5th, 4 octave
  int out = 0;             // 0 board speaker, 1 phone, 2 both
  int vol = 200;           // board speaker volume 0-255
  int chan = 1;            // MIDI channel 1-16
  char ssid[33] = ""; char pass[65] = "";
  int scale = 0;           // 0 chromatic, 1 major, 2 minor, 3 major pentatonic, 4 minor pentatonic (key = lowest note)
  int move = 0;            // how notes are picked: 0 swivel (angle), 1 slide (move the foot left/right)
  float sthr = 0.12f;      // slide: sideways push (g) that counts as a slide
  float scm = 5.0f;        // slide: cm of movement per note (0 = one note per slide)
  float rx = 1, ry = 0, rz = 0; bool rSet = false;   // slide: board direction that points to the foot's right (learned)
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
  syncWifi();
}

static const int INT_SEMIS[5] = { 3, 4, 5, 7, 12 };          // m3, M3, 4, 5, 8
static const char* INT_NAMES[5] = { "m3", "M3", "4", "5", "8" };
static const char* NOTE_NAMES[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
void noteName(int n, char* out) { sprintf(out, "%s%d", NOTE_NAMES[((n % 12) + 12) % 12], n / 12 - 1); }

volatile int otaRequest = 0;
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
bool loSet = false, hiSet = false;
char lastName[8] = "--";

float keyHalf() { return cfg.kspan * cfg.kdeg / 2; }
float keyMid() { return cfg.kEdge ? (cfg.kLo + cfg.kHi) / 2 : 0; }
// Each note is kdeg degrees wide; the range is centred on keyMid(). Edges set the centre, the direction
// (low end -> high end) and a starting kdeg; Swap reverses it either way.
// Scales: semitones above the key for each step. "Notes in range" counts steps of the chosen scale.
static const int SC_LEN[5] = { 12, 7, 7, 5, 5 };
static const int SC_STEPS[5][12] = { { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 }, { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 } };
int stepToNote(int i) { int sc = constrain(cfg.scale, 0, 4), n = SC_LEN[sc]; return cfg.kroot + 12 * (i / n) + SC_STEPS[sc][i % n]; }
int keyForTilt(float a) {
  float dir = (cfg.kEdge ? (cfg.kHi >= cfg.kLo ? 1.0f : -1.0f) : 1.0f) * (cfg.kflip ? -1.0f : 1.0f);
  float h = keyHalf(), t = constrain((a - keyMid()) * dir, -h, h);
  return stepToNote((int)roundf((t + h) / cfg.kdeg));
}

int slideStep = 9;   // slide mode: which step of the range the foot is on

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
void notesOn(int root, float vel, float swivel) {
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
  yawDeg = constrain(yawDeg, keyMid() - keyHalf() - 15, keyMid() + keyHalf() + 15);
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
          int dir = slDir * (cfg.kflip ? -1 : 1);
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
  float oct = (stepToNote(cfg.kspan) - cfg.kroot) / 12.0f;
  cfg.scale = constrain(ns, 0, 4); cfg.kspan = constrain((int)roundf(oct * SC_LEN[cfg.scale]), 2, 48);
  slideStep = constrain(slideStep, 0, cfg.kspan); keyTip = -1; notesOff();
}
void sendCfg(AsyncWebSocketClient* c) {
  char b[600];
  snprintf(b, sizeof b, "{\"cfg\":{\"thr\":%.2f,\"gap\":%d,\"kroot\":%d,\"kspan\":%d,\"kdeg\":%.1f,\"kflip\":%d,\"kflat\":%.0f,\"kEdge\":%d,\"kLo\":%.1f,\"kHi\":%.1f,\"ints\":%d,\"out\":%d,\"vol\":%d,\"chan\":%d,\"scale\":%d,\"move\":%d,\"sthr\":%.2f,\"scm\":%.1f,\"rSet\":%d,\"ver\":\"%s\",\"ssid\":\"%s\"}}",
           cfg.thr, cfg.gap, cfg.kroot, cfg.kspan, cfg.kdeg, cfg.kflip, cfg.kflat, cfg.kEdge ? 1 : 0, cfg.kLo, cfg.kHi, cfg.ints, cfg.out, cfg.vol, cfg.chan, cfg.scale, cfg.move, cfg.sthr, cfg.scm, cfg.rSet ? 1 : 0, FW_VERSION, cfg.ssid);
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
  if (strstr(s, "\"keyCenter\"")) { yawDeg = keyMid(); slideStep = cfg.kspan / 2; return; }
  if (strstr(s, "\"learnRight\"")) { learnUntil = millis() + 3000; lrnT0 = 0; return; }
  if (strstr(s, "\"keyLo\"") || strstr(s, "\"keyHi\"")) {
    bool lo = strstr(s, "\"keyLo\"") != nullptr;
    if (lo) { cfg.kLo = yawDeg; loSet = true; } else { cfg.kHi = yawDeg; hiSet = true; }
    cfg.kEdge = (loSet || cfg.kEdge) && (hiSet || cfg.kEdge) && fabsf(cfg.kHi - cfg.kLo) >= 6;
    if (cfg.kEdge) cfg.kdeg = constrain(fabsf(cfg.kHi - cfg.kLo) / max(1, cfg.kspan), 1.0f, 15.0f);
    saveCfg(); sendCfg(nullptr); ws.textAll(lo ? "{\"edge\":\"lo\"}" : "{\"edge\":\"hi\"}"); return;
  }
  if (strstr(s, "\"keyClear\"")) { cfg.kEdge = false; cfg.kLo = cfg.kHi = 0; loSet = hiSet = false; yawDeg = 0; saveCfg(); sendCfg(nullptr); return; }
  if (strstr(s, "\"allOff\"")) { keyTip = -1; notesOff(); return; }
  if (strstr(s, "\"switchFw\"")) { otaRequest = 2; return; }
  if (strstr(s, "\"ota\"")) { otaRequest = 1; return; }
  if (const char* q = strstr(s, "\"ssid\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t m = min((size_t)(e - q), sizeof(cfg.ssid) - 1); memcpy(cfg.ssid, q, m); cfg.ssid[m] = 0; } }
  if (const char* q = strstr(s, "\"pass\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t m = min((size_t)(e - q), sizeof(cfg.pass) - 1); memcpy(cfg.pass, q, m); cfg.pass[m] = 0; } }
  float v;
  v = jsonNum(s, "thr", &f); if (f) cfg.thr = constrain(v, 0.05f, 8.0f);
  v = jsonNum(s, "gap", &f); if (f) cfg.gap = constrain((int)v, 60, 600);
  v = jsonNum(s, "kroot", &f); if (f) { cfg.kroot = constrain((int)v, 24, 96); keyTip = -1; notesOff(); }
  v = jsonNum(s, "kspan", &f); if (f) cfg.kspan = constrain((int)v, 2, 48);
  v = jsonNum(s, "kdeg", &f); if (f) cfg.kdeg = constrain(v, 1.0f, 15.0f);
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
  v = jsonNum(s, "move", &f); if (f) { cfg.move = (int)v ? 1 : 0; slideStep = cfg.kspan / 2; slState = 0; }
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
  if (final) { if (Update.end(true)) otaStatus("Upload installed. Restarting…"); else otaStatus("Upload failed (is it the .app.bin?)."); }
}
void setupWeb() {
  WiFi.mode(WIFI_AP);
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
void drawScreen() {
  auto& d = M5.Display;
  char aim[8]; noteName(aimNote(), aim);
  d.startWrite(); d.fillScreen(TFT_BLACK);
  d.setTextColor(rootHeld >= 0 ? TFT_ORANGE : TFT_WHITE, TFT_BLACK); d.setTextSize(4);
  d.setCursor(4, 4); d.print(rootHeld >= 0 ? lastName : aim);
  d.setTextSize(1); d.setTextColor(TFT_DARKGREY);
  char lo[8], hi[8]; noteName(cfg.kroot, lo); noteName(stepToNote(cfg.kspan), hi);
  d.setCursor(4, 44); static const char* SCN[5] = { "chrom", "major", "minor", "maj pent", "min pent" };
  d.printf("%s-%s %s %.1fdeg", lo, hi, SCN[constrain(cfg.scale, 0, 4)], cfg.kdeg);
  d.setCursor(4, 56); d.print("+ ");
  bool any = false; for (int i = 0; i < 5; i++) if (cfg.ints & (1 << i)) { d.printf("%s ", INT_NAMES[i]); any = true; }
  if (!any) d.print("no harmony");
  d.setCursor(4, 72); d.printf("BLE %s  WiFi %d  batt %d%%", bleConnected ? "on" : "--", WiFi.softAPgetStationNum(), M5.Power.getBatteryLevel());
  d.setCursor(4, 84); d.printf("A=centre  side=scale  v%s", FW_VERSION);
  d.endWrite();
}

void setup() {
  auto c = M5.config(); c.internal_imu = true; c.internal_spk = true;
  M5.begin(c);
  M5.Display.setRotation(3); M5.Display.setBrightness(96);   // rotation 3: USB-C end away from the player
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
    if (!screenOn) { screenSet(true); drawScreen(); wakeOnly = true; }
  }
  if (screenOn && now - lastButtonMs > SCREEN_TIMEOUT_MS) screenSet(false);
  if (M5.BtnB.wasReleased() && !wakeOnly) {   // side button: next scale (chromatic > major > minor > major pent > minor pent)
    setScale((cfg.scale + 1) % 5); saveCfg(); sendCfg(nullptr); lastDraw = 0;
  }
  if (M5.BtnA.wasReleased()) { if (!wakeOnly && !resetHeld) { yawDeg = keyMid(); slideStep = cfg.kspan / 2; } resetHeld = false; }
  if ((M5.BtnA.wasReleased() || M5.BtnB.wasReleased()) && wakeOnly) wakeOnly = false;
  if (M5.BtnA.pressedFor(2000) && !resetHeld) {   // hold front button 2 s: reset settings, keep Wi-Fi
    resetHeld = true; Cfg d; memcpy(d.ssid, cfg.ssid, sizeof d.ssid); memcpy(d.pass, cfg.pass, sizeof d.pass); cfg = d; saveCfg(); sendCfg(nullptr);
  }

  float ax, ay, az, wx = 0, wy = 0, wz = 0;
  if (M5.Imu.update() && M5.Imu.getAccel(&ax, &ay, &az)) { M5.Imu.getGyro(&wx, &wy, &wz); processMotion(ax, ay, az, wx, wy, wz, now); }
  powerCheck(now, wx, wy, wz);

  if (otaRequest) { int r = otaRequest; otaRequest = 0; if (r == 2) doOtaFromGithub(OTHER_URL, OTHER_VER_URL, true); else doOtaFromGithub(OTA_URL, OTA_VER_URL, false); WiFi.mode(WIFI_AP); }

  if (ws.count() && nMeter >= 12 && now - lastMeterSend > 25) {
    char b[240]; int p = snprintf(b, sizeof b, "{\"m\":[");
    for (int i = 0; i < nMeter; i++) p += snprintf(b + p, sizeof b - p, "%s%.2f", i ? "," : "", meterBuf[i]);
    snprintf(b + p, sizeof b - p, "],\"yaw\":%.1f,\"tip\":%.0f,\"step\":%d,\"sa\":%.2f}", yawDeg, flatTilt, slideStep, saPeak); saPeak = 0; ws.textAll(b); nMeter = 0; lastMeterSend = now;
  } else if (nMeter >= 12) nMeter = 0;

  if (now - lastDraw > 250) {
    lastDraw = now; if (screenOn) drawScreen();
    static uint32_t lastBatt = 0;
    if (ws.count() && now - lastBatt > 2000) { lastBatt = now; char b[32]; snprintf(b, sizeof b, "{\"batt\":%d}", M5.Power.getBatteryLevel()); ws.textAll(b); }
    ws.cleanupClients();
  }
}
