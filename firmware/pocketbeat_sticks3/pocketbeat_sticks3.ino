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

// ---------------- Settings (persisted) ----------------
struct Cfg {
  float thr = 1.8f;      // stomp threshold: impact above this (g) = kick, g (linear accel magnitude)
  float sthr = 3.5f;     // stomp line in tap/stomp modes: harder than this (g) = stomp
  float kthr = 1.8f;     // kick threshold in bounce/stomp/lift mode; lighter landings are hats
  float swthr = 200.0f;  // swing threshold: rotation along the learned swing (deg/s) = snare
  int gap = 120;         // ms between hits
  int snare = 6;         // mode: 6 = stomp kick / swing snare, 2 = tap kick / stomp snare, 1 = tap snare / stomp kick. 0 swing right, 1 tap=snare/stomp=kick, 2 tap=kick/stomp=snare, 3 high lift, 4 bounce=hat/stomp=kick/lift=snare
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
  float lf[6] = {0}; float rf[6] = {0};            // learned left/right leg signatures (Leg mode)
  bool lcal = false, rcal = false;
  int legSwap = 0;                                 // 0: left = kick, right = snare; 1: swapped
  int sigVer = 0;                                  // 2 = leg signatures use the pre-stomp tilt
} cfg;
Preferences prefs;

void saveCfg() {
  prefs.begin("pb", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}
void loadCfg() {
  prefs.begin("pb", true);
  size_t n = prefs.getBytesLength("cfg");
  if (n > 0 && n <= sizeof(cfg)) prefs.getBytes("cfg", &cfg, n);   // older builds stored fewer fields; new ones keep defaults
  if (cfg.sigVer != 3) {   // settings from before the shoe-mode revamp: thresholds and calibration meant other things
    Cfg d; cfg.thr = d.thr; cfg.sthr = 3.5f; cfg.swthr = d.swthr; cfg.gap = d.gap; cfg.cal = false; cfg.snare = 6; cfg.sigVer = 3;
  }
  if (cfg.snare != 1 && cfg.snare != 2 && cfg.snare != 6) cfg.snare = 6;
  prefs.end();
}

volatile int otaRequest = 0;   // 1 = check+update from GitHub (run from loop, not from the websocket task)

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
    char buf[160]; int n = snprintf(buf, sizeof buf, "{\"hit\":\"%s\",\"v\":%.2f,\"g\":%.2f", kindName[k], vel, g);
    if (sw >= 0) n += snprintf(buf + n, sizeof buf - n, ",\"sw\":%.1f", sw);
    if (lift >= 0) n += snprintf(buf + n, sizeof buf - n, ",\"lift\":%.0f", lift);
    snprintf(buf + n, sizeof buf - n, "}");
    ws.textAll(buf);
  }
}

#include "pb_clock.h"

// ---------------- Motion detection ----------------
// Board taped to the top of the shoe.
//  Kick  = a stomp: an impact spike in the accelerometer above cfg.thr.
//  Snare = a swing: the shoe rotating along the learned swing direction (gyro) faster than cfg.swthr.
//          Only that direction (and sign) counts, so the toe lift before a stomp and the swing back don't trigger it.
float gx = 0, gy = 0, gz = 0;        // gravity estimate (g units)
float peak = 0; uint32_t peakT = 0, lastStompT = 0;
uint32_t lastSwingT = 0; bool inSwing = false; float swingPeak = 0;
float meterBuf[12]; int nMeter = 0; uint32_t lastMeterSend = 0;
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
  if (cfg.snare == 6) { onHit(KICK, constrain(0.6f + 0.4f * (p / (cfg.thr * 2.5f)), 0, 1), p, -1, now); return; }
  // tap/stomp modes: the stomp line decides; mode 2 = tap kick / stomp snare, mode 1 = the reverse
  bool stomp = p >= cfg.sthr;
  Kind k = (stomp ^ (cfg.snare == 1)) ? SNARE : KICK;
  float vel = stomp ? constrain(0.7f + 0.3f * (p / (cfg.sthr * 2)), 0, 1) : constrain(0.6f + 0.4f * (p / cfg.sthr), 0, 1);
  onHit(k, vel, p, -1, now);
}

void processMotion(float ax, float ay, float az, float wx, float wy, float wz, uint32_t now) {  // accel in g (incl. gravity), gyro in deg/s
  lastT = now;
  const float k = 0.05f; gx += k * (ax - gx); gy += k * (ay - gy); gz += k * (az - gz);
  float lx = ax - gx, ly = ay - gy, lz = az - gz;               // linear accel, g
  float mag = sqrtf(lx * lx + ly * ly + lz * lz);
  float wm = sqrtf(wx * wx + wy * wy + wz * wz);               // rotation speed, deg/s
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
  if (cfg.cal && cfg.snare == 6) {
    float proj = wx * cfg.rx + wy * cfg.ry + wz * cfg.rz;       // deg/s along the swing
    bool clearOfStomp = now - lastStompT > 120;                  // a stomp's own jolt can spin the board; ignore that
    if (proj > cfg.swthr && !inSwing && clearOfStomp && now - lastSwingT > (uint32_t)cfg.gap) {
      inSwing = true; lastSwingT = now; swingPeak = proj;
      onHit(SNARE, constrain(0.65f + 0.35f * (proj / (cfg.swthr * 2.5f)), 0, 1), 0, proj, now);
    }
    if (inSwing && proj < cfg.swthr * 0.3f) inSwing = false;
  }

  // ---- kick: impact spike; not right after a swing (the foot landing from a swing isn't a stomp) ----
  if (mag > cfg.thr) {
    if (now - lastStompT > (uint32_t)cfg.gap && now - lastSwingT > 250) {
      if (mag > peak) { peak = mag; if (!peakT) peakT = now; }
      if (now - peakT > 35) { float p = peak; peak = 0; peakT = 0; lastStompT = now; fireStomp(p, now); }
    }
  } else if (peakT) { float p = peak; peak = 0; peakT = 0; lastStompT = now; fireStomp(p, now); }
}

// ---------------- Web ----------------
void sendCfg(AsyncWebSocketClient* c) {
  char b[400];
  snprintf(b, sizeof b, "{\"cfg\":{\"snare\":%d,\"thr\":%.3f,\"sthr\":%.3f,\"swthr\":%.0f,\"gap\":%d,\"kit\":%d,\"out\":%d,\"quant\":%d,\"hat\":%d,\"vol\":%d,\"cal\":%d,\"ver\":\"%s\",\"ssid\":\"%s\"}}",
           cfg.snare, cfg.thr, cfg.sthr, cfg.swthr, cfg.gap, cfg.kit, cfg.out, cfg.quant, cfg.hat, cfg.vol, cfg.cal ? 1 : 0, FW_VERSION, cfg.ssid);
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
  if (strstr(s, "\"flipSwing\"")) { cfg.rx = -cfg.rx; cfg.ry = -cfg.ry; cfg.rz = -cfg.rz; saveCfg(); ws.textAll("{\"learn\":\"flipped ✓\"}"); return; }
  if (strstr(s, "\"ota\"")) { otaRequest = 1; return; }
  if (const char* q = strstr(s, "\"ssid\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t n = min((size_t)(e - q), sizeof(cfg.ssid) - 1); memcpy(cfg.ssid, q, n); cfg.ssid[n] = 0; } }
  if (const char* q = strstr(s, "\"pass\":\"")) { q += 8; const char* e = strchr(q, '"'); if (e) { size_t n = min((size_t)(e - q), sizeof(cfg.pass) - 1); memcpy(cfg.pass, q, n); cfg.pass[n] = 0; } }
  float v;
  v = jsonNum(s, "thr", &f); if (f) cfg.thr = v;
  v = jsonNum(s, "sthr", &f); if (f) cfg.sthr = v;
  v = jsonNum(s, "swthr", &f); if (f) cfg.swthr = v;
  v = jsonNum(s, "lthr", &f); if (f) cfg.lthr = v;
  v = jsonNum(s, "kthr", &f); if (f) cfg.kthr = v;
  v = jsonNum(s, "gap", &f); if (f) cfg.gap = (int)v;
  v = jsonNum(s, "snare", &f); if (f && (v == 1 || v == 2 || v == 6)) cfg.snare = (int)v;
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
void doOtaFromGithub() {
  otaStatus("Joining home Wi-Fi…");
  if (!joinHomeWifi()) return;
  WiFiClientSecure client; client.setInsecure();   // GitHub over TLS without a bundled root cert
  HTTPClient http;
  otaStatus("Checking latest version…");
  http.begin(client, OTA_VER_URL); int code = http.GET();
  String remote = code == 200 ? http.getString() : ""; remote.trim(); http.end();
  if (code != 200) { otaStatus("Could not reach GitHub."); return; }
  if (remote == FW_VERSION) { char b[96]; snprintf(b, sizeof b, "Already up to date (%s).", FW_VERSION); otaStatus(b); return; }
  otaStatus("Downloading update…");
  http.begin(client, OTA_URL); code = http.GET();
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
uint32_t lastDraw = 0, lastButtonMs = 0; bool screenOn = true;
const uint32_t SCREEN_TIMEOUT_MS = 30000; bool wakeOnly = false, resetHeld = false;
void screenSet(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  if (on) { M5.Display.wakeup(); M5.Display.setBrightness(96); }
  else { M5.Display.setBrightness(0); M5.Display.sleep(); }
}
void drawScreen() {
  auto& d = M5.Display;
  d.startWrite(); d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK); d.setTextSize(2);
  d.setCursor(4, 4); d.printf("%d bpm", beatMs ? (int)roundf(60000.0f / beatMs) : 0);
  d.setCursor(4, 30); d.setTextColor(TFT_ORANGE); d.print(lastKind);
  d.setTextSize(1); d.setTextColor(TFT_DARKGREY);
  d.setCursor(4, 60); d.printf("BLE %s  WiFi %d", bleConnected ? "on" : "--", WiFi.softAPgetStationNum());
  d.setCursor(4, 72); d.printf("batt %d%%  v%s", M5.Power.getBatteryLevel(), FW_VERSION);
  d.setCursor(4, 84); d.print(calibrating ? "swing foot 3x..." : cfg.snare == 2 ? "tap=kick stomp=snare" : cfg.snare == 1 ? "tap=snare stomp=kick" : cfg.cal ? "stomp=kick swing=snare" : "SET SWING: press A");
  d.endWrite();
}

void setup() {
  auto c = M5.config(); c.internal_imu = true;
#ifndef PB_LITE
  c.internal_spk = true;
#endif
  M5.begin(c);
  M5.Display.setRotation(1); M5.Display.setBrightness(96);
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
  if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) {
    lastButtonMs = now;
    if (!screenOn) { screenSet(true); drawScreen(); wakeOnly = true; }
  }
  if (screenOn && now - lastButtonMs > SCREEN_TIMEOUT_MS) screenSet(false);
  // Button A: calibrate the "right" direction. Hold 2 s: factory reset.
  if (M5.BtnA.wasReleased()) { if (wakeOnly) wakeOnly = false; else if (!resetHeld && cfg.snare == 6) startSwingCal(); resetHeld = false; }
  if (M5.BtnA.pressedFor(2000) && !resetHeld) {   // reset settings, keep Wi-Fi
    resetHeld = true; Cfg d; memcpy(d.ssid, cfg.ssid, sizeof d.ssid); memcpy(d.pass, cfg.pass, sizeof d.pass); d.sigVer = 3; cfg = d; saveCfg(); sendCfg(nullptr);
  }

  float ax, ay, az;
  float wx = 0, wy = 0, wz = 0;
  if (M5.Imu.update() && M5.Imu.getAccel(&ax, &ay, &az)) { M5.Imu.getGyro(&wx, &wy, &wz); processMotion(ax, ay, az, wx, wy, wz, now); }

  if (otaRequest) { otaRequest = 0; doOtaFromGithub(); WiFi.mode(WIFI_AP); }
  if (pending.on && now >= pending.at) { pending.on = false; hit(pending.k, pending.vel, pending.g, pending.sw, pending.lift); }
  clockTick(now);

  // meter stream to the page, ~40 Hz, a dozen samples per message
  if (ws.count() && nMeter >= 12 && now - lastMeterSend > 25) {
    char b[160]; int p = snprintf(b, sizeof b, "{\"m\":[");
    for (int i = 0; i < nMeter; i++) p += snprintf(b + p, sizeof b - p, "%s%.2f", i ? "," : "", meterBuf[i]);
    snprintf(b + p, sizeof b - p, "]}"); ws.textAll(b); nMeter = 0; lastMeterSend = now;
  } else if (nMeter >= 12) nMeter = 0;

  if (now - lastDraw > 500) {
    lastDraw = now; if (screenOn) drawScreen();
    if (ws.count()) { char b[32]; snprintf(b, sizeof b, "{\"batt\":%d}", M5.Power.getBatteryLevel()); ws.textAll(b); }
    ws.cleanupClients();
  }
}
