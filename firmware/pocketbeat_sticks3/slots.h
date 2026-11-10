// Shared by PocketBeat, Footboard and ChiPet (keep the copies identical).
//
// Three firmwares on one board. The flash (partitions.csv, same in every sketch folder) has three app slots.
// Each firmware carries a marker string ("\x7fFWID:<id>:<version>:") so any firmware can tell what sits in the
// other slots by scanning them; nothing depends on NVS, so a fresh USB flash just works.
//   - switchTo(id): boot the slot that holds that firmware (no download)
//   - installFromGithub(id): fetch that firmware's .app.bin into a free slot (or over an older copy of itself)
//   - upload (page): same, from a file on the phone
//   - a self-update goes into a free slot too; if none is free it overwrites one and remembers to put that
//     firmware back (NVS flag) once home Wi-Fi is up.
// Also here: the home Wi-Fi manager (short, spaced attempts so the board's own network stays up).
#pragma once
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#ifndef FW_ID
#error "define FW_ID (\"pb\", \"fb\" or \"pet\") before including slots.h"
#endif
// The marker lives in this firmware's image; other firmwares find it by scanning the slot.
const char FW_MARK[] __attribute__((used, section(".rodata.fwmark"))) = "\x7f" "FWID:" FW_ID ":" FW_VERSION ":";

namespace slots {

// ---- boot guard: if the firmware dies three times in a row within 40 s of starting, the next boot is "safe mode"
// (no Bluetooth, no Wi-Fi scanning, no motion processing): the board's own network and the page still work, so
// you can update or switch firmware instead of reaching for the USB cable.
static bool safeMode = false;
static bool bootGuard() {
  Preferences p; p.begin("boot", false);
  int crashes = p.getInt("c", 0);
  esp_reset_reason_t why = esp_reset_reason();
  bool crashed = (why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT || why == ESP_RST_BROWNOUT);
  if (crashed) crashes++; else if (why == ESP_RST_POWERON) crashes = 0;
  safeMode = crashes >= 3;
  if (safeMode) crashes = 0;   // one safe boot, then try normally again
  p.putInt("c", crashes); p.end();
  return safeMode;
}
static void bootOk() { Preferences p; p.begin("boot", false); if (p.getInt("c", 0)) p.putInt("c", 0); p.end(); }   // call once after 40 s alive

struct Fw { const char* id; const char* name; const char* host; const char* ap; const char* bin; const char* ver; };
static const Fw FWS[3] = {
  { "pb", "PocketBeat", "pocketbeat", "PocketBeat", "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/pocketbeat_sticks3.app.bin", "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/version.txt" },
  { "fb", "Footboard", "footboard", "Footboard", "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/footboard_sticks3.app.bin", "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/footboard_version.txt" },
  { "pet", "ChiPet", "chipet", "ChiPet", "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/chipet_sticks3.app.bin", "https://raw.githubusercontent.com/millennialsbandvb-max/ai/main/firmware/bin/chipet_version.txt" },
};
static const Fw* fwById(const char* id) { for (auto& f : FWS) if (!strcmp(f.id, id)) return &f; return nullptr; }

typedef void (*StatusFn)(const char*);
static StatusFn status = nullptr;
static void say(const char* s) { if (status) status(s); }

// ---- what is in each slot ----
struct SlotInfo { const esp_partition_t* part; char id[8]; char ver[16]; bool valid; bool running; };
static SlotInfo info[3]; static int nSlots = 0; static int runningSlot = -1;

static bool scanSlot(const esp_partition_t* p, char* id, char* ver) {
  // look for "\x7fFWID:" (built at run time so this code's own copy of the text is never found)
  char needle[7]; needle[0] = 0x7f; memcpy(needle + 1, "FWID:", 6);
  static uint8_t buf[4096 + 32];
  esp_app_desc_t d; if (esp_ota_get_partition_description(p, &d) != ESP_OK) return false;   // no valid image
  size_t carry = 0;
  for (size_t off = 0; off < p->size; off += 4096) {
    size_t n = min((size_t)4096, (size_t)(p->size - off));
    if (esp_partition_read(p, off, buf + carry, n) != ESP_OK) return false;
    size_t tot = carry + n;
    for (size_t i = 0; i + 6 < tot; i++) {
      if (buf[i] == 0x7f && !memcmp(buf + i, needle, 6)) {
        // need the rest of the marker: "<id>:<ver>:" — refetch a small window from flash to be safe
        uint8_t w[48]; size_t at = off - carry + i; size_t wn = min((size_t)48, (size_t)(p->size - at));
        if (esp_partition_read(p, at, w, wn) != ESP_OK) return false;
        char* s = (char*)w + 6; char* c1 = (char*)memchr(s, ':', wn - 6); if (!c1) return false;
        char* c2 = (char*)memchr(c1 + 1, ':', wn - 6 - (c1 + 1 - s)); if (!c2) return false;
        size_t li = min((size_t)(c1 - s), (size_t)7), lv = min((size_t)(c2 - c1 - 1), (size_t)15);
        memcpy(id, s, li); id[li] = 0; memcpy(ver, c1 + 1, lv); ver[lv] = 0; return true;
      }
    }
    carry = min(tot, (size_t)16); memmove(buf, buf + tot - carry, carry);
  }
  return false;
}
static void rescan() {
  nSlots = 0; const esp_partition_t* run = esp_ota_get_running_partition();
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it && nSlots < 3) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 && p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_2) {
      SlotInfo& s = info[nSlots]; s.part = p; s.running = (p->address == run->address); s.id[0] = 0; s.ver[0] = 0;
      if (s.running) { strlcpy(s.id, FW_ID, sizeof s.id); strlcpy(s.ver, FW_VERSION, sizeof s.ver); s.valid = true; }
      else s.valid = scanSlot(p, s.id, s.ver);
      if (s.running) runningSlot = nSlots;
      nSlots++;
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
}
static int slotWith(const char* id) { for (int i = 0; i < nSlots; i++) if (info[i].valid && !strcmp(info[i].id, id)) return i; return -1; }
// slot to write a firmware into: its own old copy > an empty/invalid slot > the slot holding the firmware that
// was installed longest ago (we just pick the first non-running one and remember to restore it)
static int targetFor(const char* id, const char** evicted) {
  *evicted = nullptr;
  int s = slotWith(id); if (s >= 0 && !info[s].running) return s;
  for (int i = 0; i < nSlots; i++) if (!info[i].running && !info[i].valid) return i;
  for (int i = 0; i < nSlots; i++) if (!info[i].running) { *evicted = info[i].id; return i; }
  return -1;
}

static void setRestore(const char* id) { Preferences p; p.begin("slots", false); p.putString("restore", id ? id : ""); p.end(); }
static String restorePending() { Preferences p; p.begin("slots", true); String v = p.getString("restore", ""); p.end(); return v; }

static void begin(StatusFn st) { status = st; esp_ota_mark_app_valid_cancel_rollback(); rescan(); }

static bool switchTo(const char* id) {
  int s = slotWith(id); if (s < 0) return false;
  if (esp_ota_set_boot_partition(info[s].part) != ESP_OK) return false;
  char b[80]; snprintf(b, sizeof b, "Switching to %s… restarting.", fwById(id)->name); say(b);
  delay(600); ESP.restart(); return true;
}

// ---- writing a slot ----
static esp_ota_handle_t wh = 0; static int wslot = -1; static size_t wdone = 0; static bool wfail = false; static const char* wevict = nullptr; static char wid[8];
static bool writeBegin(const char* id, size_t size) {
  rescan(); wslot = targetFor(id, &wevict); wdone = 0; wfail = false; strlcpy(wid, id, sizeof wid);
  if (wslot < 0) return false;
  if (esp_ota_begin(info[wslot].part, size ? size : OTA_SIZE_UNKNOWN, &wh) != ESP_OK) { wslot = -1; return false; }
  return true;
}
static bool writeChunk(const uint8_t* d, size_t n) {
  if (wslot < 0 || wfail) return false;
  if (esp_ota_write(wh, d, n) != ESP_OK) { wfail = true; esp_ota_abort(wh); wslot = -1; return false; }
  wdone += n; return true;
}
// boot = switch to it right after (self-update / upload-and-run)
static bool writeEnd(bool boot) {
  if (wslot < 0 || wfail) return false;
  bool ok = esp_ota_end(wh) == ESP_OK;
  int s = wslot; wslot = -1;
  if (!ok) { rescan(); return false; }
  if (wevict && strcmp(wevict, wid)) setRestore(wevict);   // we overwrote another firmware: put it back later
  rescan();
  if (boot) { esp_ota_set_boot_partition(info[s].part); delay(600); ESP.restart(); }
  return true;
}
static void writeAbort() { if (wslot >= 0) { esp_ota_abort(wh); wslot = -1; } }

// Download a firmware from GitHub into a slot. Needs home Wi-Fi up. force: even if that version is already there.
static bool installFromGithub(const char* id, bool force, bool boot) {
  const Fw* f = fwById(id); if (!f) return false;
  if (WiFi.status() != WL_CONNECTED) { say("Not on home Wi-Fi: save it below, then try again."); return false; }
  WiFiClientSecure client; client.setInsecure(); HTTPClient http; char b[150];
  snprintf(b, sizeof b, "Checking %s version…", f->name); say(b);
  int code = 0; String remote;
  for (int t = 0; t < 3 && code != 200; t++) {
    if (t) delay(1500);
    http.begin(client, f->ver); code = http.GET(); remote = code == 200 ? http.getString() : ""; remote.trim(); http.end();
  }
  if (code != 200) { snprintf(b, sizeof b, "Could not reach GitHub (error %d). Try again, or use your phone's hotspot.", code); say(b); return false; }
  int have = slotWith(id);
  if (!force && have >= 0 && remote == info[have].ver) { snprintf(b, sizeof b, "%s is already up to date (%s).", f->name, remote.c_str()); say(b); return boot ? switchTo(id) : true; }
  snprintf(b, sizeof b, "Downloading %s %s…", f->name, remote.c_str()); say(b);
  http.begin(client, f->bin); code = http.GET();
  if (code != 200) { http.end(); snprintf(b, sizeof b, "Download failed (error %d).", code); say(b); return false; }
  int len = http.getSize();
  if (!writeBegin(id, len > 0 ? len : 0)) { http.end(); say("No slot to write into."); return false; }
  WiFiClient* s = http.getStreamPtr(); uint8_t buf[2048]; uint32_t lastMsg = 0;
  while (http.connected() && (len < 0 || wdone < (size_t)len)) {
    size_t n = s->available(); if (!n) { delay(1); continue; }
    n = s->readBytes(buf, min(n, sizeof buf));
    if (!writeChunk(buf, n)) { http.end(); say("Write failed."); return false; }
    if (millis() - lastMsg > 500) { snprintf(b, sizeof b, "Installing %s… %d%%", f->name, len > 0 ? (int)(wdone * 100 / len) : 0); say(b); lastMsg = millis(); }
  }
  http.end();
  if (boot) say("Installed. Restarting…");
  if (!writeEnd(boot)) { say("The downloaded file was not a valid firmware."); return false; }
  snprintf(b, sizeof b, "%s %s installed. Use Switch to boot it.", f->name, remote.c_str()); say(b);
  return true;
}

// After an update that overwrote another firmware: put it back once Wi-Fi is up (call from loop).
static bool restoreTick(bool wifiUp) {
  String r = restorePending(); if (!r.length()) return false;
  if (!wifiUp) return true;   // pending
  if (installFromGithub(r.c_str(), true, false)) setRestore(nullptr);
  else setRestore(nullptr);   // don't loop forever on a bad download; the page can retry by hand
  return false;
}

// JSON fragment for the page: "slots":[{"id":"pb","name":..,"ver":"..","run":1},...],"restore":"fb"
static void json(char* out, size_t n) {
  int p = snprintf(out, n, "\"slots\":[");
  for (int i = 0; i < nSlots && p < (int)n; i++) {
    const Fw* f = info[i].valid ? fwById(info[i].id) : nullptr;
    p += snprintf(out + p, n - p, "%s{\"id\":\"%s\",\"name\":\"%s\",\"ver\":\"%s\",\"run\":%d}", i ? "," : "", info[i].valid ? info[i].id : "", f ? f->name : (info[i].valid ? info[i].id : ""), info[i].valid ? info[i].ver : "", info[i].running ? 1 : 0);
  }
  String r = restorePending();
  snprintf(out + p, n - p, "],\"restore\":\"%s\",\"fws\":[\"pb\",\"fb\",\"pet\"]", r.c_str());
}

// ---- home Wi-Fi manager: remembers every network you save (up to 8) and joins whichever is in range ----
struct Net { char ssid[33]; char pass[65]; };
static Net nets[8]; static int nNets = 0; static const char* nHost = "board";
static uint32_t attemptStart = 0, nextAttempt = 0; static bool attempting = false, mdnsUp = false, wasUp = false, scanning = false;
static char curSsid[33] = "";
static const uint32_t ATTEMPT_MS = 20000, RETRY_MS = 120000;
static void netLoad() {
  Preferences p; p.begin("wifi", true); nNets = constrain((int)p.getInt("n", 0), 0, 8);
  for (int i = 0; i < nNets; i++) { char k[6]; snprintf(k, sizeof k, "s%d", i); strlcpy(nets[i].ssid, p.getString(k, "").c_str(), 33); snprintf(k, sizeof k, "p%d", i); strlcpy(nets[i].pass, p.getString(k, "").c_str(), 65); }
  p.end();
}
static void netStore() {
  Preferences p; p.begin("wifi", false); p.putInt("n", nNets);
  for (int i = 0; i < nNets; i++) { char k[6]; snprintf(k, sizeof k, "s%d", i); p.putString(k, nets[i].ssid); snprintf(k, sizeof k, "p%d", i); p.putString(k, nets[i].pass); }
  p.end();
}
static void netAdd(const char* ssid, const char* pass) {   // newest first; same name replaces
  if (!ssid || !ssid[0]) return;
  int at = -1; for (int i = 0; i < nNets; i++) if (!strcmp(nets[i].ssid, ssid)) at = i;
  if (at < 0) { if (nNets < 8) nNets++; at = nNets - 1; }
  for (int i = at; i > 0; i--) nets[i] = nets[i - 1];
  strlcpy(nets[0].ssid, ssid, 33); strlcpy(nets[0].pass, pass ? pass : "", 65);
  netStore();
}
static void netForget(const char* ssid) {
  for (int i = 0; i < nNets; i++) if (!strcmp(nets[i].ssid, ssid)) { for (int k = i; k < nNets - 1; k++) nets[k] = nets[k + 1]; nNets--; netStore(); if (!strcmp(curSsid, ssid)) WiFi.disconnect(); return; }
}
static void netBegin(const char* host, const char* ssid, const char* pass) {
  nHost = host; netLoad();
  if (ssid && ssid[0]) { bool have = false; for (int i = 0; i < nNets; i++) if (!strcmp(nets[i].ssid, ssid)) have = true; if (!have) netAdd(ssid, pass); }   // carry over the single saved one
  WiFi.setAutoReconnect(false); WiFi.setSleep(false);
  nextAttempt = millis() + 20000;   // give the phone 20 s to join the board's own network first
}
static void netSet(const char* ssid, const char* pass) { netAdd(ssid, pass); WiFi.disconnect(); attempting = false; scanning = false; nextAttempt = millis() + 200; }
// pick the strongest saved network among the scan results; -1 if none in range
static int netBest(int found) {
  int best = -1, bestRssi = -1000;
  for (int r = 0; r < found; r++) { String ss = WiFi.SSID(r); for (int i = 0; i < nNets; i++) if (ss == nets[i].ssid && WiFi.RSSI(r) > bestRssi) { bestRssi = WiFi.RSSI(r); best = i; } }
  return best;
}
static void netConnectTo(int i, uint32_t now) { strlcpy(curSsid, nets[i].ssid, 33); WiFi.begin(nets[i].ssid, nets[i].pass); attempting = true; attemptStart = now; }
static void netTick(uint32_t now, bool pageOpen) {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up) { attempting = false; scanning = false; if (!mdnsUp) { mdnsUp = MDNS.begin(nHost); if (mdnsUp) MDNS.addService("http", "tcp", 80); } wasUp = true; return; }
  if (mdnsUp) { MDNS.end(); mdnsUp = false; }
  if (wasUp) { wasUp = false; WiFi.disconnect(); attempting = false; nextAttempt = now + 5000; }
  if (scanning) {   // a background scan is running: when it's done, join the best saved network it found
    int found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) return;
    scanning = false; int b = found > 0 ? netBest(found) : -1; WiFi.scanDelete();
    if (b >= 0) netConnectTo(b, now); else nextAttempt = now + RETRY_MS;
    return;
  }
  if (attempting) { if (now - attemptStart > ATTEMPT_MS) { WiFi.disconnect(); attempting = false; nextAttempt = now + RETRY_MS; } return; }
  if (nNets && (int32_t)(now - nextAttempt) >= 0 && !pageOpen) { WiFi.scanNetworks(true); scanning = true; }
}
static bool netJoinNow(uint32_t waitMs = 15000) {
  if (!nNets) { say("No Wi-Fi saved yet. Enter one below first."); return false; }
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(); delay(100); scanning = false;
  say("Looking for a saved Wi-Fi…");
  int found = WiFi.scanNetworks(); int b = found > 0 ? netBest(found) : -1; WiFi.scanDelete();
  if (b < 0) { attempting = false; nextAttempt = millis() + RETRY_MS; say("None of your saved Wi-Fi networks is in range (save this place's Wi-Fi or a hotspot below)."); return false; }
  netConnectTo(b, millis());
  uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) delay(100);
  if (WiFi.status() != WL_CONNECTED) { WiFi.disconnect(); attempting = false; nextAttempt = millis() + RETRY_MS; char m[120]; snprintf(m, sizeof m, "Could not join %s (wrong password?).", nets[b].ssid); say(m); return false; }
  attempting = false; return true;
}
static bool netUp() { return WiFi.status() == WL_CONNECTED; }
static const char* netCurrent() { return WiFi.status() == WL_CONNECTED ? curSsid : ""; }
static void netJson(char* out, size_t n) {   // "nets":["a","b"]
  int p = snprintf(out, n, "\"nets\":[");
  for (int i = 0; i < nNets && p < (int)n - 40; i++) p += snprintf(out + p, n - p, "%s\"%s\"", i ? "," : "", nets[i].ssid);
  snprintf(out + p, n - p, "]");
}

}  // namespace slots
