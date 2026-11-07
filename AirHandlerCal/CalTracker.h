// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// CalTracker.h — the bath calibration's bookkeeping: which pair is plugged in, whether its reading has
// settled, whether the bath is flat, and when enough has been logged. No hardware here, so
// test/tracker_test.cpp runs it on the Mac.

#pragma once

#include <stdint.h>

class CalTracker {
public:
  enum Status { OUT, SETTLE, NO_PROBE, SOAK, LOG, DONE };

  static const uint32_t OPEN_MIN_MS = 1000;     // both channels open this long means the plug is out
  static const uint32_t IN_MIN_MS   = 500;      // both channels closed this long means the next plug is in
  static const uint32_t SETTLE_MS   = 15000;    // after a plug goes in, before its lines count
  static const uint32_t SOAK_MS     = 300000;   // the bath must stay flat this long before lines count
  static const int      FLAT_LINES  = 30;       // lines in the flatness window: 60 s at one line per 2 s
  static const int      WANT_LINES  = 60;       // 2 min at one line per 2 s

  // Flatness of the probes' mean over the window. The 10-20 baths held a two-probe mean within
  // 0.115 F over any 60 s once soaked, and a mean of ten moves less.
  static constexpr float FLAT_F = 0.15f;

  explicit CalTracker(int pairs) : nPairs(pairs) {}

  // Every ADC sample. Advances the pair when a plug comes out and the next goes in. A plug is out once
  // both channels have been open for OPEN_MIN_MS, and the next is in once both have been closed for
  // IN_MIN_MS: a contact that flickers while the plug is pushed home (2026-11-01, every touch of a
  // half-seated PTSM plug advanced the pair again) counts for nothing until it holds.
  void sample(uint32_t now, bool retOpen, bool supOpen) {
    bool bothOpen = retOpen && supOpen, bothClosed = !retOpen && !supOpen;
    if (bothOpen) {
      if (!openTiming) { openTiming = true; openSince = now; }
      if (now - openSince >= OPEN_MIN_MS && !plugOut) { plugOut = true; hadPlug = everPlugged; }
      inTiming = false;
    } else {
      openTiming = false;
      if (bothClosed) {
        if (!inTiming) { inTiming = true; inSince = now; }
        if (plugOut && now - inSince >= IN_MIN_MS) {
          plugOut = false;
          if (hadPlug) {                        // the first plug after boot is pair 1 as it stands
            pair = pair % nPairs + 1;
            goodLines = 0;
          }
        }
        everPlugged = true;
      } else {
        inTiming = false;                       // one channel open: a half-seated plug
      }
    }
    if (retOpen || supOpen) {
      closed = false;
      windowOpen = true;
    } else if (!closed) {
      closed = true;
      closedSince = now;
    }
  }

  // Once per log line. mean is the probes' mean in F, allProbes false when any probe failed to read
  // or there are none. Returns this line's status.
  Status line(uint32_t now, float mean, bool allProbes) {
    bool wasOpen = windowOpen;
    windowOpen = false;
    if (!allProbes) {
      goodLines = 0;
      return closed ? NO_PROBE : OUT;
    }
    pushMean(now, mean);
    if (!closed || wasOpen) return OUT;
    if (now - closedSince < SETTLE_MS) return SETTLE;
    if (!flat || now - flatSince < SOAK_MS) return SOAK;
    if (goodLines < WANT_LINES) goodLines++;
    return goodLines >= WANT_LINES ? DONE : LOG;
  }

  void setPair(int p) {
    if (p < 1 || p > nPairs) return;
    pair = p;
    goodLines = 0;
  }

  // After a rescan: the mean of a different set of probes is not comparable with the old one.
  void resetBath() {
    filled = 0;
    next = 0;
    flat = false;
    goodLines = 0;
  }

  int currentPair() const { return pair; }
  int linesLogged() const { return goodLines; }
  int pairs() const { return nPairs; }

  static const char *name(Status s) {
    switch (s) {
      case OUT:      return "out";
      case SETTLE:   return "settle";
      case NO_PROBE: return "noprobe";
      case SOAK:     return "soak";
      case LOG:      return "log";
      default:       return "done";
    }
  }

private:
  int      nPairs;
  int      pair = 1;
  bool     everPlugged = false;
  bool     plugOut = false;
  bool     openTiming = false;
  uint32_t openSince = 0;
  bool     inTiming = false;
  uint32_t inSince = 0;
  bool     hadPlug = false;                     // a plug had been in before the current out
  bool     closed = false;
  uint32_t closedSince = 0;
  bool     windowOpen = false;

  float    means[FLAT_LINES] = {0};
  int      filled = 0, next = 0;
  bool     flat = false;
  uint32_t flatSince = 0;
  int      goodLines = 0;

  void pushMean(uint32_t now, float mean) {
    means[next] = mean;
    next = (next + 1) % FLAT_LINES;
    if (filled < FLAT_LINES) filled++;
    float lo = means[0], hi = means[0];
    for (int i = 1; i < filled; i++) {
      if (means[i] < lo) lo = means[i];
      if (means[i] > hi) hi = means[i];
    }
    bool nowFlat = filled == FLAT_LINES && hi - lo <= FLAT_F;
    if (nowFlat && !flat) flatSince = now;
    if (!nowFlat) goodLines = 0;                // the bath moved: this pair starts again
    flat = nowFlat;
  }
};
