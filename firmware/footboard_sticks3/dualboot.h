// Shared by PocketBeat and Footboard (keep the two copies identical).
//
// 1. Dual boot: the 8 MB flash has two app slots. One holds this firmware, the other can hold the other one.
//    "Switch" just boots the other slot (no download); "Install the other firmware" fills the other slot from
//    GitHub or from an uploaded .app.bin without booting it. A self-update necessarily overwrites the other
//    slot, so after it the new build re-downloads the other firmware into the freed slot (flag in NVS).
//    Which firmware sits in which slot is kept in NVS ("slots"), written by whoever writes a slot and
//    confirmed by each firmware at boot.
// 2. Home Wi-Fi: the board keeps its own network up and joins the saved home Wi-Fi/hotspot in the background.
//    A station that keeps failing to connect makes the ESP32 scan constantly, which knocks the board's own
//    network off the air, so attempts are short and spaced out, and never made while someone is on the page.
#pragma once
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace dual {

typedef void (*StatusFn)(const char*);
static const char* ME = "";
static const char* OTHER = "";
static const char* ME_NAME = "";
static const char* OTHER_NAME = "";
static const char* OTHER_URL = nullptr;
static const char* OTHER_VER_URL = nullptr;
static StatusFn status = nullptr;
static char meVersion[16] = "";

static int slotOf(const esp_partition_t* p) { return p ? (int)(p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : -1; }
static const esp_partition_t* runningPart() { return esp_ota_get_running_partition(); }
static const esp_partition_t* otherPart() { return esp_ota_get_next_update_partition(nullptr); }

static void regSet(int slot, const char* app, const char* ver) {
  Preferences p; p.begin("slots", false);
  char k[8]; snprintf(k, sizeof k, "app%d", slot); p.putString(k, app);
  snprintf(k, sizeof k, "ver%d", slot); p.putString(k, ver ? ver : "");
  p.end();
}
static String regGet(const char* pre, int slot) {
  Preferences p; p.begin("slots", true);
  char k[8]; snprintf(k, sizeof k, "%s%d", pre, slot); String v = p.getString(k, ""); p.end(); return v;
}

// ---- slot state ----
static bool otherInstalled() {
  const esp_partition_t* o = otherPart(); if (!o) return false;
  esp_app_desc_t d; if (esp_ota_get_partition_description(o, &d) != ESP_OK) return false;   // no valid image there
  return regGet("app", slotOf(o)) == OTHER;
}
static String otherVersion() { return otherInstalled() ? regGet("ver", slotOf(otherPart())) : String(""); }
static bool reinstallPending() { Preferences p; p.begin("slots", true); bool v = p.getBool("reinst", false); p.end(); return v; }
static void setReinstall(bool v) { Preferences p; p.begin("slots", false); p.putBool("reinst", v); p.end(); }

static void begin(const char* me, const char* meName, const char* other, const char* otherName, const char* otherUrl, const char* otherVerUrl, const char* version, StatusFn st) {
  ME = me; ME_NAME = meName; OTHER = other; OTHER_NAME = otherName; OTHER_URL = otherUrl; OTHER_VER_URL = otherVerUrl; status = st;
  strlcpy(meVersion, version, sizeof meVersion);
  esp_ota_mark_app_valid_cancel_rollback();   // in case rollback is enabled: we booted fine
  regSet(slotOf(runningPart()), ME, version);
}

// Before rebooting into a freshly self-updated image (which went into the other slot): remember that the
// other firmware has to be put back into the slot we are leaving.
static void beforeSelfUpdateReboot() {
  regSet(slotOf(otherPart()), ME, "");   // the new build of us is there now
  setReinstall(true);
}

static bool switchToOther() {
  if (!otherInstalled()) return false;
  if (esp_ota_set_boot_partition(otherPart()) != ESP_OK) return false;
  if (status) { char b[80]; snprintf(b, sizeof b, "Switching to %s… restarting.", OTHER_NAME); status(b); }
  delay(600); ESP.restart(); return true;
}

// ---- writing the other slot without booting it ----
static esp_ota_handle_t wh = 0; static const esp_partition_t* wp = nullptr; static size_t wdone = 0; static bool wfail = false;
static bool writeBegin(size_t size) {
  wp = otherPart(); wdone = 0; wfail = false; if (!wp) return false;
  if (esp_ota_begin(wp, size ? size : OTA_SIZE_UNKNOWN, &wh) != ESP_OK) { wp = nullptr; return false; }
  regSet(slotOf(wp), "writing", "");
  return true;
}
static bool writeChunk(const uint8_t* d, size_t n) {
  if (!wp || wfail) return false;
  if (esp_ota_write(wh, d, n) != ESP_OK) { wfail = true; esp_ota_abort(wh); wp = nullptr; return false; }
  wdone += n; return true;
}
static bool writeEnd(const char* ver) {
  if (!wp || wfail) return false;
  bool ok = esp_ota_end(wh) == ESP_OK;   // validates the image
  if (ok) regSet(slotOf(wp), OTHER, ver); else regSet(slotOf(wp), "bad", "");
  wp = nullptr; return ok;
}
static void writeAbort() { if (wp) { esp_ota_abort(wh); regSet(slotOf(wp), "bad", ""); wp = nullptr; } }

// Download the other firmware from GitHub into the other slot. Needs home Wi-Fi up (caller joins it).
static bool installOther() {
  if (WiFi.status() != WL_CONNECTED) { if (status) status("Not on home Wi-Fi: save it below, then try again."); return false; }
  WiFiClientSecure client; client.setInsecure(); HTTPClient http;
  char b[140];
  snprintf(b, sizeof b, "Checking %s version…", OTHER_NAME); if (status) status(b);
  int code = 0; String remote;
  for (int t = 0; t < 3 && code != 200; t++) {
    if (t) delay(1500);
    http.begin(client, OTHER_VER_URL); code = http.GET(); remote = code == 200 ? http.getString() : ""; remote.trim(); http.end();
  }
  if (code != 200) { snprintf(b, sizeof b, "Could not reach GitHub (error %d).", code); if (status) status(b); return false; }
  snprintf(b, sizeof b, "Downloading %s %s…", OTHER_NAME, remote.c_str()); if (status) status(b);
  http.begin(client, OTHER_URL); code = http.GET();
  if (code != 200) { http.end(); snprintf(b, sizeof b, "Download failed (error %d).", code); if (status) status(b); return false; }
  int len = http.getSize();
  if (!writeBegin(len > 0 ? len : 0)) { http.end(); if (status) status("Could not open the other slot."); return false; }
  WiFiClient* s = http.getStreamPtr(); uint8_t buf[2048]; uint32_t lastMsg = 0;
  while (http.connected() && (len < 0 || wdone < (size_t)len)) {
    size_t n = s->available(); if (!n) { delay(1); continue; }
    n = s->readBytes(buf, min(n, sizeof buf));
    if (!writeChunk(buf, n)) { http.end(); if (status) status("Write failed."); return false; }
    if (millis() - lastMsg > 500) { snprintf(b, sizeof b, "Installing %s… %d%%", OTHER_NAME, len > 0 ? (int)(wdone * 100 / len) : 0); if (status) status(b); lastMsg = millis(); }
  }
  http.end();
  if (!writeEnd(remote.c_str())) { if (status) status("The downloaded file was not a valid firmware."); return false; }
  snprintf(b, sizeof b, "%s %s installed alongside. Use Switch to boot it.", OTHER_NAME, remote.c_str()); if (status) status(b);
  return true;
}

static void json(char* out, size_t n) {
  String ov = otherVersion();
  snprintf(out, n, "\"me\":\"%s\",\"slot\":%d,\"other\":\"%s\",\"otherOk\":%d,\"otherVer\":\"%s\",\"reinst\":%d",
           ME, slotOf(runningPart()), OTHER, otherInstalled() ? 1 : 0, ov.c_str(), reinstallPending() ? 1 : 0);
}

// ---- home Wi-Fi manager ----
static char nSsid[33] = "", nPass[65] = ""; static const char* nHost = "board";
static uint32_t attemptStart = 0, nextAttempt = 0; static bool attempting = false, mdnsUp = false, wasUp = false;
static const uint32_t ATTEMPT_MS = 20000, RETRY_MS = 120000;

static void netBegin(const char* host, const char* ssid, const char* pass) {
  nHost = host; strlcpy(nSsid, ssid, sizeof nSsid); strlcpy(nPass, pass, sizeof nPass);
  WiFi.setAutoReconnect(false); WiFi.setSleep(false);
  nextAttempt = millis() + 1500;   // first try shortly after boot
}
static void netSet(const char* ssid, const char* pass) {   // new credentials from the page: try now
  strlcpy(nSsid, ssid, sizeof nSsid); strlcpy(nPass, pass, sizeof nPass);
  WiFi.disconnect(); attempting = false; nextAttempt = millis() + 200;
}
static void netStartAttempt(uint32_t now) {
  if (!nSsid[0]) return;
  WiFi.begin(nSsid, nPass); attempting = true; attemptStart = now;
}
// pageOpen: someone is on the board's own network / page -> don't start scans that would drop them
static void netTick(uint32_t now, bool pageOpen) {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up) {
    attempting = false;
    if (!mdnsUp) { mdnsUp = MDNS.begin(nHost); if (mdnsUp) MDNS.addService("http", "tcp", 80); }
    wasUp = true; return;
  }
  if (mdnsUp) { MDNS.end(); mdnsUp = false; }
  if (wasUp) { wasUp = false; WiFi.disconnect(); attempting = false; nextAttempt = now + 5000; }   // dropped: retry soon
  if (attempting) {
    if (now - attemptStart > ATTEMPT_MS) { WiFi.disconnect(); attempting = false; nextAttempt = now + RETRY_MS; }   // give up, keep own network clean
    return;
  }
  if (nSsid[0] && (int32_t)(now - nextAttempt) >= 0 && !pageOpen) netStartAttempt(now);
}
// Blocking join for updates (the page asked, so a short scan is fine).
static bool netJoinNow(uint32_t waitMs = 15000) {
  if (!nSsid[0]) { if (status) status("No home Wi-Fi set. Enter it below first."); return false; }
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(); delay(100); WiFi.begin(nSsid, nPass); attempting = true; attemptStart = millis();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(); attempting = false; nextAttempt = millis() + RETRY_MS; if (status) status("Could not join home Wi-Fi (check name/password, or use your phone's hotspot)."); return false; }
  attempting = false; return true;
}
static bool netUp() { return WiFi.status() == WL_CONNECTED; }

}  // namespace dual
