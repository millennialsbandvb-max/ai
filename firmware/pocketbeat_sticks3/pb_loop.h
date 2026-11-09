// Drum looper for the swivel kit. Included after pb_clock.h.
//
// The loop is cfg.lbars bars of 16th-note slots riding on the beat clock's 8th grid (two slots per 8th).
// Hats you play set the tempo (pb_clock.h); the moment the clock starts, the hats you already played are
// dropped into the loop at their quantized places. While recording is on, every kick/snare/hat you play is
// added at its quantized slot (one of each drum per slot, newest velocity wins). The loop plays back on the
// grid; auto hats are off in loop mode because the hats come from the loop.
//
// Gestures (heel lift = foot tipped from its resting angle, see the sketch):
//   quick lift (< 0.4 s)      play / pause
//   hold 0.4 - 1.8 s          recording on / off
//   hold > 1.8 s              erase the drum of the zone you're swivelled to (hats / kick / snare)
//   violent shake 3 beats     stop and clear everything (pb_clock stop + loopClear)
#pragma once

const int LOOP_MAX_SLOTS = 64;                 // 4 bars of 16ths
const int LOOP_KINDS = 4;                      // KICK, SNARE, HAT, OHAT
uint8_t loopVel[LOOP_MAX_SLOTS][LOOP_KINDS];   // 0 = nothing, else velocity 1..127
int32_t loopArmed[LOOP_MAX_SLOTS][LOOP_KINDS]; // slot counter at which this event was recorded (don't replay it in the same pass)
bool loopPlaying = true, loopRec = true;
int32_t loopLastCounter = -1;                  // last global slot counter we played
float hatHist[16]; int nHatHist = 0;           // recent hand-played hat times (before the clock existed)
uint8_t hatHistOpen[16];

static int loopKindIdx(Kind k) { return k == KICK ? 0 : k == SNARE ? 1 : k == HAT ? 2 : k == OHAT ? 3 : -1; }
static Kind loopIdxKind(int i) { return i == 0 ? KICK : i == 1 ? SNARE : i == 2 ? HAT : OHAT; }
int loopSlots() { return constrain(cfg.lbars, 1, 4) * 16; }
bool loopHasContent() { for (int s = 0; s < loopSlots(); s++) for (int k = 0; k < LOOP_KINDS; k++) if (loopVel[s][k]) return true; return false; }
void loopClear(int kindIdx = -1) {   // -1 = everything, 2 clears both hats
  for (int s = 0; s < LOOP_MAX_SLOTS; s++) for (int k = 0; k < LOOP_KINDS; k++)
    if (kindIdx < 0 || k == kindIdx || (kindIdx == 2 && k == 3)) { loopVel[s][k] = 0; loopArmed[s][k] = -1; }
}
void loopSendState() {
  if (!ws.count()) return;
  char b[LOOP_MAX_SLOTS * LOOP_KINDS + 96]; int p = snprintf(b, sizeof b, "{\"loop\":{\"rec\":%d,\"play\":%d,\"slots\":%d,\"v\":\"", loopRec ? 1 : 0, loopPlaying ? 1 : 0, loopSlots());
  for (int s = 0; s < loopSlots(); s++) for (int k = 0; k < LOOP_KINDS; k++) b[p++] = loopVel[s][k] ? ('0' + min(9, loopVel[s][k] / 13)) : '.';
  p += snprintf(b + p, sizeof b - p, "\"}}"); ws.textAll(b);
}
// global slot counter (two 16ths per 8th) for a time near now; the 8th just passed is eighthIdx-1 at lastEighth
static int32_t loopCounterAt(float t) {
  if (!clockOn || beatMs <= 0) return -1;
  float e = beatMs / 2;
  return (int32_t)(eighthIdx - 1) * 2 + (int32_t)roundf((t - lastEighth) / (e / 2));
}
static int loopSlotOf(int32_t counter) { int n = loopSlots(); return ((counter % n) + n) % n; }

void loopRecord(Kind k, float vel, float t) {
  int ki = loopKindIdx(k); if (ki < 0 || !clockOn || !loopRec) return;
  int32_t c = loopCounterAt(t); if (c < 0) return;
  int s = loopSlotOf(c);
  loopVel[s][ki] = (uint8_t)constrain(20 + vel * 107, 1, 127);
  loopArmed[s][ki] = c;   // if the playhead reaches this very counter, skip it: the live hit already sounded
  loopSendState();
}
// hats played before the tempo existed: remember them, drop them in once the clock starts
void loopNoteHatTime(float t, bool open) {
  if (nHatHist < 16) { hatHist[nHatHist] = t; hatHistOpen[nHatHist] = open; nHatHist++; }
  else { memmove(hatHist, hatHist + 1, 15 * sizeof(float)); memmove(hatHistOpen, hatHistOpen + 1, 15); hatHist[15] = t; hatHistOpen[15] = open; }
}
void loopBackfillHats(float now) {
  if (!clockOn || !loopRec) return;
  float span = loopSlots() * beatMs / 4;   // one loop length
  for (int i = 0; i < nHatHist; i++) if (now - hatHist[i] < span) {
    int32_t c = loopCounterAt(hatHist[i]); int s = loopSlotOf(c); int ki = hatHistOpen[i] ? 3 : 2;
    if (!loopVel[s][ki]) { loopVel[s][ki] = 90; loopArmed[s][ki] = c; }
  }
  nHatHist = 0; loopSendState();
}
// play back: called every loop() pass
void loopTick(float now) {
  if (!clockOn || beatMs <= 0) return;
  float e = beatMs / 2;
  int32_t c = (int32_t)(eighthIdx - 1) * 2 + (int32_t)floorf((now - lastEighth) / (e / 2));
  if (loopLastCounter < 0 || c - loopLastCounter > 8) loopLastCounter = c - 1;   // (re)anchored: don't catch up on a burst of old slots
  while (loopLastCounter < c) {
    loopLastCounter++;
    if (!loopPlaying) continue;
    int s = loopSlotOf(loopLastCounter);
    for (int k = 0; k < LOOP_KINDS; k++) if (loopVel[s][k] && loopArmed[s][k] != loopLastCounter) hit(loopIdxKind(k), loopVel[s][k] / 127.0f);
  }
  static uint32_t lastPos = 0; uint32_t pos = (uint32_t)millis();
  if (ws.count() && pos - lastPos > 100) { lastPos = pos; char b[48]; snprintf(b, sizeof b, "{\"lpos\":%d}", loopSlotOf(c)); ws.textAll(b); }
}
