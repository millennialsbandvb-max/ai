// Beat clock: tempo + hi-hat engine.
// Included from the sketch after cfg, hit(), ws and Kind are defined (and from the host test harness).
//
// The clock runs an 8th-note grid. Tempo is learned from the gaps between hits, measured as a
// whole number of 8th notes (half beat, beat, beat and a half, two beats...), so syncopated kicks
// on the "&" and short rests confirm the tempo instead of breaking it. Each hit nudges the grid
// toward where it landed, spread over the next few hats so the hats never lurch.
#pragma once

float beatMs = 0; uint32_t lastHitMs = 0, firstHitMs = 0; bool clockOn = false;
float nextEighth = 0, lastEighth = 0, drift = 0, lastHatPlayed = -1e9; uint32_t eighthIdx = 0;
int offGrid = 0;                       // consecutive hits that didn't fit the grid
float gapBuf[4]; int nGaps = 0;        // recent raw gaps between hits (ms), newest last
int hitsSinceStart = 0;
bool locked = false; int lockHits = 0; float countStart = 0;   // Locked tempo: count-in hits so far, first count-in hit, and whether the tempo is held
const int COUNT_IN = 8;

static float foldBeat(float ms) {      // put a gap into a normal beat range (70-160 bpm)
  if (ms <= 0) return 0;
  while (ms < 375) ms *= 2;
  while (ms > 860) ms /= 2;
  return ms;
}
// Find the beat length (70-160 bpm) whose 8th notes explain all the given gaps best.
// When several fit, the longest beat wins (the simplest reading of the rhythm).
static float solveBeat(const float* g, int n) {
  float best = 0, bestErr = 1e9;
  for (float B = 860; B >= 375; B -= 2) {
    float e = B / 2, err = 0;
    for (int i = 0; i < n; i++) { float k = roundf(g[i] / e); if (k < 1) { err += 1; continue; } err += fabsf(g[i] - k * e) / e; }
    if (err < bestErr - 0.04f * n) { bestErr = err; best = B; }
  }
  return best;
}
static void pushGap(float g) { if (nGaps < 4) gapBuf[nGaps++] = g; else { memmove(gapBuf, gapBuf + 1, 3 * sizeof(float)); gapBuf[3] = g; } }
void startClock(float anchor, float now) {
  float e = beatMs / 2;
  eighthIdx = 0; lastEighth = anchor;
  while (lastEighth + e <= now) { lastEighth += e; eighthIdx++; }
  nextEighth = lastEighth + e; drift = 0; offGrid = 0; clockOn = true;
}
void stopClock() { clockOn = false; lastHitMs = 0; firstHitMs = 0; beatMs = 0; offGrid = 0; nGaps = 0; locked = false; lockHits = 0; ws.textAll("{\"bpm\":0}"); }

void clockTick(float now) {
  if (!clockOn) return;
  // Follow mode: stop after about two and a half beats of silence. Locked tempo: after two bars.
  if (now - lastHitMs > beatMs * (cfg.tlock ? 8.0f : 2.5f)) { stopClock(); return; }
  float e = beatMs / 2;
  while (nextEighth <= now) {
    if (drift != 0) { float d = drift * 0.35f; nextEighth += d; drift -= d; if (fabsf(drift) < 1) drift = 0; }
    bool onBeat = (eighthIdx % 2) == 0;
    bool play = cfg.hat == 2 || (cfg.hat == 1 && onBeat);
    // never two hats closer than ~60% of an 8th (re-locking the grid can otherwise double one up)
    if (play && now - nextEighth < 40 && nextEighth - lastHatPlayed > 0.6f * e) {
      hit(cfg.ohat && hatIsOpen ? OHAT : HAT, onBeat ? 0.7f : 0.45f);   // toe raised past the angle: open hat
      lastHatPlayed = nextEighth;
    }
    lastEighth = nextEighth; eighthIdx++; nextEighth += e;
  }
}
// Nudge the grid toward a hit that landed near an 8th line (bigger misses are left alone).
void nudgeGrid(float now) {
  float e = beatMs / 2;
  float nearest = lastEighth + roundf((now - lastEighth) / e) * e;
  float err = now - nearest;
  if (fabsf(err) < 0.3f * e) drift = constrain(0.5f * err, -0.2f * e, 0.2f * e);
}

// Pending quantized hit (held back to the next grid line, at most 90 ms)
struct Pending { bool on = false; float at = 0; Kind k; float vel, g, sw, lift; } pending;

void onHit(Kind k, float vel, float g, float sw, float now, float lift = -1) {
  float when = now;
  if (cfg.tlock) {
    if (locked && clockOn) {
      // Locked: the tempo and the hats' timing are fixed; hits just play (quantized if on).
      if (cfg.quant) {
        float gsub = beatMs / cfg.quant;
        float grid = lastEighth + roundf((now - lastEighth) / gsub) * gsub;
        if (grid - now < 90 && grid >= now - 5) when = grid;
      }
    } else {
      // Count-in: 8 evenly spaced hits. The tempo is the total time of the 8 hits divided into 7 beats.
      if (lockHits > 0 && now - lastHitMs > 2000) lockHits = 0;   // too long a gap: start counting again
      if (++lockHits == 1) countStart = now;
      if (lockHits >= COUNT_IN) {
        beatMs = (now - countStart) / (COUNT_IN - 1);
        locked = true;
        startClock(now, now);                                       // the 8th hit is a beat; hats start from here
        lastEighth = now - beatMs / 2; nextEighth = now; eighthIdx = 0;   // first hat lands with the 8th hit
      }
      if (ws.count()) {
        char b[64];
        if (locked) snprintf(b, sizeof b, "{\"bpm\":%d,\"locked\":1}", (int)roundf(60000.0f / beatMs));
        else snprintf(b, sizeof b, "{\"count\":%d}", lockHits);
        ws.textAll(b);
      }
    }
    lastHitMs = now;
    if (k == TAP) return;
    if (when > now + 1) { pending = { true, when, k, vel, g, sw, lift }; }
    else hit(k, vel, g, sw, lift);
    return;
  }
  if (!lastHitMs) {
    firstHitMs = now; nGaps = 0;                                 // first hit: remember it, wait for the second
  } else {
    float dt = now - lastHitMs;
    if (dt > 150 && dt < 2000) pushGap(dt);
    if (!beatMs) {
      if (dt > 150 && dt < 2000) { beatMs = foldBeat(dt); startClock(lastHitMs, now); hitsSinceStart = 0; }   // second hit starts the clock
      else { firstHitMs = now; nGaps = 0; }
    } else {
      // Third hit: now there are two gaps, so re-read the tempo from both (a first gap of a beat and a
      // half, or half a beat, can't be read correctly on its own).
      if (++hitsSinceStart == 1 && nGaps >= 2) {
        float B = solveBeat(gapBuf + nGaps - 2, 2);
        if (B && fabsf(B - beatMs) / beatMs > 0.08f) { beatMs = B; startClock(firstHitMs, now); }
      }
      float e = beatMs / 2;
      int n = (int)roundf(dt / e);                               // gap in 8th notes
      if (n >= 1 && n <= 8 && fabsf(dt - n * e) < 0.2f * e) {   // must land within 20% of an 8th of the grid
        float cand = 2 * dt / n;                                 // the beat length this gap implies
        float w = n >= 2 ? 0.25f : 0.15f;                         // longer gaps measure tempo better
        beatMs += w * (cand - beatMs);
        if (offGrid > 0) offGrid--;
      } else if (dt > 150 && dt < 2000) {
        if (++offGrid >= 3 && nGaps >= 3) {                        // clearly a new tempo: relearn from the last three gaps
          float B = solveBeat(gapBuf + nGaps - 3, 3); if (B) { beatMs = B; startClock(now, now); }
        }
      }
      if (cfg.quant) {
        float gsub = beatMs / cfg.quant;
        float grid = lastEighth + roundf((now - lastEighth) / gsub) * gsub;
        if (grid - now < 90 && grid >= now - 5) when = grid;
      }
      if (!clockOn) startClock(now, now); else nudgeGrid(now);
    }
  }
  lastHitMs = now;
  if (ws.count()) { char b[48]; snprintf(b, sizeof b, "{\"bpm\":%d,\"locked\":%d}", beatMs ? (int)roundf(60000.0f / beatMs) : 0, locked ? 1 : 0); ws.textAll(b); }
  if (k == TAP) { if (ws.count()) { char b[64]; snprintf(b, sizeof b, "{\"hit\":\"tap\",\"v\":0,\"g\":%.2f,\"lift\":%.0f}", g, lift); ws.textAll(b); } return; }
  if (when > now + 1) { pending = { true, when, k, vel, g, sw, lift }; }
  else hit(k, vel, g, sw, lift);
}
