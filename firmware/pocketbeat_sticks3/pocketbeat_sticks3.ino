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

// ---------------- Settings (persisted) ----------------
struct Cfg {
  float thr = 0.8f;      // lightest landing that counts, g (linear accel magnitude)
  float sthr = 2.5f;     // hard stomp threshold, g (stomp modes)
  float kthr = 1.8f;     // kick threshold in bounce/stomp/lift mode; lighter landings are hats
  float swthr = 0.35f;   // swing speed threshold, m/s
  int gap = 120;         // ms between hits
  int snare = 3;         // 0 swing right, 1 tap=snare/stomp=kick, 2 tap=kick/stomp=snare, 3 high lift, 4 bounce=hat/stomp=kick/lift=snare
  float lthr = 35.0f;    // lift threshold, degrees of rotation since the foot last went still
  int kit = 0;           // 0 acoustic, 1 electro
  int out = 0;           // 0 board speaker, 1 phone, 2 both
  int quant = 2;         // 0 off, 2 eighths, 4 sixteenths
  int hat = 2;           // 0 off, 1 quarters, 2 eighths (auto hats)
  int vol = 255;         // speaker volume 0-255
  float rx = 0, ry = 0, rz = 0; bool cal = false;  // "right" direction in sensor frame
} cfg;
Preferences prefs;

void saveCfg() {
  prefs.begin("pb", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}
void loadCfg() {
  prefs.begin("pb", true);
  if (prefs.getBytesLength("cfg") == sizeof(cfg)) prefs.getBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}

// ---------------- Outputs ----------------
BLEMIDI_CREATE_INSTANCE("PocketBeat", MIDI);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
bool bleConnected = false;

const char* kindName[] = { "kick", "snare", "hat", "tap" };
const char* lastKind = "";
const uint8_t midiNote[] = { 36, 38, 42, 0 };   // GM drums: kick, snare, closed hat (tap: none)

void playBoard(Kind k, float vel) {
#ifdef PB_LITE
  return;                    // lite build: no samples, no speaker (Bluetooth MIDI + phone only)
#else
  if (cfg.out == 1) return;  // phone only
  const Kit& kt = KITS[cfg.kit];
  const int16_t* d = k == KICK ? kt.kick : k == SNARE ? kt.snare : kt.hat;
  uint32_t n = k == KICK ? kt.nKick : k == SNARE ? kt.nSnare : kt.nHat;
  int ch = (int)k;  // one speaker channel per drum, so a new hit cuts the old one
  M5.Speaker.setChannelVolume(ch, (uint8_t)(cfg.vol * (k == HAT ? 0.6f : 1.0f) * (0.7f + 0.3f * vel)));
  M5.Speaker.playRaw(d, n, SMP_RATE, false, 1, ch, true);
#endif
}
void sendMidi(Kind k, float vel) {
  uint8_t v = (uint8_t)constrain(60 + vel * 67, 1, 127);
  if (bleConnected) { MIDI.sendNoteOn(midiNote[k], v, 10); MIDI.sendNoteOff(midiNote[k], 0, 10); }
#ifdef HAVE_USB_MIDI
  usbMidi.noteOn(midiNote[k], v, 10);
  usbMidi.noteOff(midiNote[k], 0, 10);
#endif
}
void hit(Kind k, float vel, float g = 0, float sw = -1, float lift = -1) {
  if (k != HAT) lastKind = kindName[k];
  playBoard(k, vel);
  sendMidi(k, vel);
  if (ws.count()) {
    char buf[128];
    if (sw >= 0) snprintf(buf, sizeof buf, "{\"hit\":\"%s\",\"v\":%.2f,\"sw\":%.2f}", kindName[k], vel, sw);
    else if (lift >= 0) snprintf(buf, sizeof buf, "{\"hit\":\"%s\",\"v\":%.2f,\"g\":%.2f,\"lift\":%.0f}", kindName[k], vel, g, lift);
    else snprintf(buf, sizeof buf, "{\"hit\":\"%s\",\"v\":%.2f,\"g\":%.2f}", kindName[k], vel, g);
    ws.textAll(buf);
  }
}

// ---------------- Beat clock (port of the web version) ----------------
// A clock starts after 3 evenly spaced hits. Hats free-run from the clock; each hit nudges
// the phase a little. Tempo follows the median of recent good gaps, slowly.
float intervals[4]; int nInt = 0; float rejected[3]; int nRej = 0;
float beatMs = 0; uint32_t lastHitMs = 0; bool clockOn = false;
float nextHat = 0, lastHatSched = 0, drift = 0; uint32_t hatIdx = 0;

float median(float* a, int n) { float b[8]; memcpy(b, a, n * sizeof(float)); for (int i = 1; i < n; i++) for (int j = i; j > 0 && b[j - 1] > b[j]; j--) { float t = b[j]; b[j] = b[j - 1]; b[j - 1] = t; } return b[(n - 1) / 2]; }
bool consistent(float* a, int n, float tol) { float m = median(a, n); for (int i = 0; i < n; i++) if (fabsf(a[i] - m) / m >= tol) return false; return true; }
void pushInt(float v) { if (nInt < 4) intervals[nInt++] = v; else { memmove(intervals, intervals + 1, 3 * sizeof(float)); intervals[3] = v; } }
float hatSub() { return cfg.hat == 2 ? beatMs / 2 : beatMs; }
void startClock(float now) { nextHat = now; lastHatSched = now; hatIdx = 0; drift = 0; clockOn = true; }
void stopClock() { clockOn = false; nInt = 0; nRej = 0; lastHitMs = 0; beatMs = 0; ws.textAll("{\"bpm\":0}"); }

void clockTick(float now) {
  if (!clockOn) return;
  if (now - lastHitMs > beatMs * 1.6f) { stopClock(); return; }       // stop soon after the foot stops
  if (cfg.hat == 0) { nextHat = now; return; }
  float sub = hatSub();
  while (nextHat <= now) {
    if (drift != 0) { float d = drift * 0.35f; nextHat += d; drift -= d; if (fabsf(drift) < 1) drift = 0; }
    bool onBeat = cfg.hat == 2 ? (hatIdx % 2 == 0) : true;
    if (now - nextHat < 40) hit(HAT, onBeat ? 0.7f : 0.45f);
    lastHatSched = nextHat; hatIdx++; nextHat += sub;
  }
}
void nudgeHats(float now) {
  float sub = hatSub();
  float nearest = lastHatSched + roundf((now - lastHatSched) / sub) * sub;
  float err = now - nearest;
  drift = constrain(0.7f * err, -0.25f * sub, 0.25f * sub);
}

// Pending quantized hit (held back to the next grid line, at most 90 ms)
struct Pending { bool on = false; float at = 0; Kind k; float vel, g, sw, lift; } pending;

void onHit(Kind k, float vel, float g, float sw, float now, float lift = -1) {
  if (lastHitMs) {
    float dt = now - lastHitMs;
    if (!beatMs) {
      if (dt > 250 && dt < 2000) { if (nInt >= 2) { intervals[0] = intervals[1]; nInt = 1; } intervals[nInt++] = dt; } else nInt = 0;
    } else {
      float r = dt / beatMs;
      if (r > 1.7f && r < 2.3f) dt /= 2;
      if (dt / beatMs > 0.7f && dt / beatMs < 1.4f) { pushInt(dt); nRej = 0; }
      else if (dt > 250 && dt < 2000) {
        if (nRej < 3) rejected[nRej++] = dt; else { rejected[0] = rejected[1]; rejected[1] = rejected[2]; rejected[2] = dt; }
        // Two off-tempo hits that agree with each other = a deliberate tempo change: jump to it.
        if (nRej >= 2 && consistent(rejected + nRej - 2, 2, 0.2f)) { intervals[0] = rejected[nRej - 2]; intervals[1] = rejected[nRej - 1]; nInt = 2; nRej = 0; beatMs = median(intervals, 2); }
      }
    }
  }
  lastHitMs = now;
  float when = now;
  if (!beatMs) {
    // Bounces start the clock on the second bounce; other hits need two even gaps.
    if (nInt >= 1 && (k == TAP || (nInt == 2 && consistent(intervals, 2, 0.2f)))) { beatMs = median(intervals, nInt); startClock(now); }
  } else {
    beatMs += 0.5f * (median(intervals, nInt) - beatMs);
    if (cfg.quant) {
      float gsub = beatMs / cfg.quant;
      float grid = lastHatSched + roundf((now - lastHatSched) / gsub) * gsub;
      if (grid - now < 90 && grid >= now - 5) when = grid;
    }
    if (!clockOn) startClock(now); else nudgeHats(now);
  }
  if (ws.count()) { char b[32]; snprintf(b, sizeof b, "{\"bpm\":%d}", beatMs ? (int)roundf(60000.0f / beatMs) : 0); ws.textAll(b); }
  if (k == TAP) { if (ws.count()) { char b[64]; snprintf(b, sizeof b, "{\"hit\":\"tap\",\"v\":0,\"g\":%.2f,\"lift\":%.0f}", g, lift); ws.textAll(b); } return; }
  if (when > now + 1) { pending = { true, when, k, vel, g, sw, lift }; }
  else hit(k, vel, g, sw, lift);
}

// ---------------- Motion detection ----------------
float gx = 0, gy = 0, gz = 0;        // gravity estimate (g units)
float peak = 0; uint32_t peakT = 0, lastStompT = 0;
float vRight = 0; uint32_t lastSwingT = 0; bool inSwing = false; float swingPeak = 0;
bool calibrating = false; uint32_t calEnd = 0; float calBest = 0, calX = 0, calY = 0, calZ = 0;
float meterBuf[12]; int nMeter = 0; uint32_t lastMeterSend = 0;
float gyroMag = 0, liftMax = 0;   // deg/s; liftMax = fastest rotation since the last landing, decays slowly
float liftAngle = 0, stillMs = 0; // degrees rotated since the foot last went still
uint32_t lastT = 0;

void fireStomp(float p, float now) {
  if (cfg.snare == 4) {   // bounce = hat, stomp = kick, lift = snare
    float lift = liftAngle; liftAngle = 0; liftMax = 0;
    if (lift >= cfg.lthr) onHit(SNARE, constrain(0.7f + 0.3f * (lift / (2 * cfg.lthr)), 0, 1), p, -1, now, lift);
    else if (p >= cfg.kthr) onHit(KICK, constrain(0.6f + 0.4f * (p / (cfg.kthr * 2)), 0, 1), p, -1, now, lift);
    else { hit(HAT, constrain(0.5f + 0.5f * (p / cfg.kthr), 0, 1), p, -1, lift); onHit(TAP, 0, p, -1, now, lift); }  // live hat + tempo tap
    return;
  }
  if (cfg.snare == 3) {   // stomp = kick, lift = snare; a light bounce (below the kick line) is a silent tempo tap
    float lift = liftAngle; liftAngle = 0; liftMax = 0;
    if (lift >= cfg.lthr) onHit(SNARE, constrain(0.7f + 0.3f * (lift / (2 * cfg.lthr)), 0, 1), p, -1, now, lift);
    else if (p >= cfg.kthr) onHit(KICK, constrain(0.6f + 0.4f * (p / (cfg.kthr * 2)), 0, 1), p, -1, now, lift);
    else onHit(TAP, 0, p, -1, now, lift);
    return;
  }
  liftMax = 0; liftAngle = 0;
  bool hardMode = cfg.snare == 1 || cfg.snare == 2, isHard = hardMode && p >= cfg.sthr;
  Kind hardKind = cfg.snare == 1 ? KICK : SNARE, softKind = cfg.snare == 1 ? SNARE : KICK;
  if (isHard) onHit(hardKind, constrain(0.7f + 0.3f * (p / (cfg.sthr * 2)), 0, 1), p, -1, now);
  else onHit(hardMode ? softKind : KICK, constrain(0.6f + 0.4f * (p / (hardMode ? cfg.sthr : cfg.thr * 2.5f)), 0, 1), p, -1, now);
}

void processMotion(float ax, float ay, float az, float wx, float wy, float wz, uint32_t now) {  // accel in g (incl. gravity), gyro in deg/s
  float dt = lastT ? min((uint32_t)50, now - lastT) / 1000.0f : 0; lastT = now;
  gyroMag = sqrtf(wx * wx + wy * wy + wz * wz);
  if (gyroMag > liftMax) liftMax = gyroMag; else liftMax *= (1.0f - 1.2f * dt);  // forgets a lift after ~1 s
  if (gyroMag > 20) { liftAngle += gyroMag * dt; stillMs = 0; } else { stillMs += dt * 1000; if (stillMs > 250) liftAngle = 0; }
  const float k = 0.05f; gx += k * (ax - gx); gy += k * (ay - gy); gz += k * (az - gz);
  float lx = ax - gx, ly = ay - gy, lz = az - gz;               // linear accel, g
  float mag = sqrtf(lx * lx + ly * ly + lz * lz);
  if (nMeter < 12) meterBuf[nMeter++] = mag;

  // horizontal part
  float gl = sqrtf(gx * gx + gy * gy + gz * gz); if (gl < 0.3f) gl = 1;
  float ux = gx / gl, uy = gy / gl, uz = gz / gl;
  float dot = lx * ux + ly * uy + lz * uz;
  float hx = lx - dot * ux, hy = ly - dot * uy, hz = lz - dot * uz;

  if (calibrating) {
    float hm = sqrtf(hx * hx + hy * hy + hz * hz);
    if (hm > calBest) { calBest = hm; calX = hx; calY = hy; calZ = hz; }
    if (now > calEnd) {
      calibrating = false;
      float m = sqrtf(calX * calX + calY * calY + calZ * calZ);
      if (calBest > 0.15f && m > 0) { cfg.rx = calX / m; cfg.ry = calY / m; cfg.rz = calZ / m; cfg.cal = true; saveCfg(); ws.textAll("{\"cal\":1}"); }
      else ws.textAll("{\"cal\":0}");
      vRight = 0;
    }
    return;
  }

  if (cfg.cal && cfg.snare == 0) {
    float aR = (hx * cfg.rx + hy * cfg.ry + hz * cfg.rz) * 9.81f;   // m/s^2 toward right
    vRight = (vRight + aR * dt) * 0.985f;
    if (mag < 0.1f) vRight *= 0.8f;
    if (vRight > cfg.swthr && !inSwing && now - lastSwingT > (uint32_t)cfg.gap) {
      inSwing = true; lastSwingT = now; swingPeak = vRight;
      onHit(SNARE, constrain(0.7f + 0.3f * (vRight / (cfg.swthr * 2)), 0, 1), 0, vRight, now);
    }
    if (inSwing) { if (vRight > swingPeak) swingPeak = vRight; if (vRight < cfg.swthr * 0.4f) inSwing = false; }
  }

  // stomp: impact spike detector
  if (mag > cfg.thr) {
    if (now - lastStompT > (uint32_t)cfg.gap && now - lastSwingT > 250) {
      if (mag > peak) { peak = mag; if (!peakT) peakT = now; }
      if (now - peakT > 35) { float p = peak; peak = 0; peakT = 0; lastStompT = now; fireStomp(p, now); }
    }
  } else if (peakT) { float p = peak; peak = 0; peakT = 0; lastStompT = now; fireStomp(p, now); }
}

// ---------------- Web ----------------
void sendCfg(AsyncWebSocketClient* c) {
  char b[320];
  snprintf(b, sizeof b, "{\"cfg\":{\"thr\":%.3f,\"sthr\":%.3f,\"swthr\":%.2f,\"lthr\":%.0f,\"kthr\":%.3f,\"gap\":%d,\"snare\":%d,\"kit\":%d,\"out\":%d,\"quant\":%d,\"hat\":%d,\"vol\":%d,\"cal\":%d}}",
           cfg.thr, cfg.sthr, cfg.swthr, cfg.lthr, cfg.kthr, cfg.gap, cfg.snare, cfg.kit, cfg.out, cfg.quant, cfg.hat, cfg.vol, cfg.cal ? 1 : 0);
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
  if (strstr(s, "\"cal\"")) { calibrating = true; calBest = 0; calEnd = millis() + 2500; return; }
  float v;
  v = jsonNum(s, "thr", &f); if (f) cfg.thr = v;
  v = jsonNum(s, "sthr", &f); if (f) cfg.sthr = v;
  v = jsonNum(s, "swthr", &f); if (f) cfg.swthr = v;
  v = jsonNum(s, "lthr", &f); if (f) cfg.lthr = v;
  v = jsonNum(s, "kthr", &f); if (f) cfg.kthr = v;
  v = jsonNum(s, "gap", &f); if (f) cfg.gap = (int)v;
  v = jsonNum(s, "snare", &f); if (f) cfg.snare = (int)v;
  v = jsonNum(s, "kit", &f); if (f) cfg.kit = constrain((int)v, 0, 1);
  v = jsonNum(s, "out", &f); if (f) cfg.out = (int)v;
  v = jsonNum(s, "quant", &f); if (f) cfg.quant = (int)v;
  v = jsonNum(s, "hat", &f); if (f) cfg.hat = (int)v;
  v = jsonNum(s, "vol", &f); if (f) { cfg.vol = constrain((int)v, 0, 255); M5.Speaker.setVolume(cfg.vol); }
  saveCfg();
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
  server.onNotFound([](AsyncWebServerRequest* r) { r->redirect("/"); });  // captive-portal style
  server.begin();
}

// ---------------- Display ----------------
uint32_t lastDraw = 0;
void drawScreen() {
  auto& d = M5.Display;
  d.startWrite(); d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK); d.setTextSize(2);
  d.setCursor(4, 4); d.printf("%d bpm", beatMs ? (int)roundf(60000.0f / beatMs) : 0);
  d.setCursor(4, 30); d.setTextColor(TFT_ORANGE); d.print(lastKind);
  d.setTextSize(1); d.setTextColor(TFT_DARKGREY);
  d.setCursor(4, 60); d.printf("BLE %s  WiFi %d", bleConnected ? "on" : "--", WiFi.softAPgetStationNum());
  d.setCursor(4, 72); d.printf("batt %d%%", M5.Power.getBatteryLevel());
  d.setCursor(4, 84); d.print(cfg.snare == 0 ? (cfg.cal ? "snare: swing R" : "snare: SET RIGHT") : cfg.snare == 1 ? "tap=sn stomp=kk" : cfg.snare == 2 ? "tap=kk stomp=sn" : cfg.snare == 3 ? "snare: high lift" : "bounce/stomp/lift");
  d.endWrite();
}

void setup() {
  auto c = M5.config(); c.internal_imu = true;
#ifndef PB_LITE
  c.internal_spk = true;
#endif
  M5.begin(c);
  M5.Display.setRotation(1);
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

  // Button A: calibrate the "right" direction. Hold 2 s: factory reset.
  if (M5.BtnA.wasReleased()) { calibrating = true; calBest = 0; calEnd = now + 2500; }
  if (M5.BtnA.pressedFor(2000)) { cfg = Cfg(); saveCfg(); sendCfg(nullptr); }

  float ax, ay, az;
  float wx = 0, wy = 0, wz = 0;
  if (M5.Imu.update() && M5.Imu.getAccel(&ax, &ay, &az)) { M5.Imu.getGyro(&wx, &wy, &wz); processMotion(ax, ay, az, wx, wy, wz, now); }

  if (pending.on && now >= pending.at) { pending.on = false; hit(pending.k, pending.vel, pending.g, pending.sw, pending.lift); }
  clockTick(now);

  // meter stream to the page, ~40 Hz, a dozen samples per message
  if (ws.count() && nMeter >= 12 && now - lastMeterSend > 25) {
    char b[160]; int p = snprintf(b, sizeof b, "{\"m\":[");
    for (int i = 0; i < nMeter; i++) p += snprintf(b + p, sizeof b - p, "%s%.2f", i ? "," : "", meterBuf[i]);
    snprintf(b + p, sizeof b - p, "]}"); ws.textAll(b); nMeter = 0; lastMeterSend = now;
  } else if (nMeter >= 12) nMeter = 0;

  if (now - lastDraw > 500) {
    lastDraw = now; drawScreen();
    if (ws.count()) { char b[32]; snprintf(b, sizeof b, "{\"batt\":%d}", M5.Power.getBatteryLevel()); ws.textAll(b); }
    ws.cleanupClients();
  }
}
