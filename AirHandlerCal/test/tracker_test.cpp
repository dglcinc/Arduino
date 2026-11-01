// Host test of CalTracker: c++ -std=c++17 -I.. tracker_test.cpp -o /tmp/t && /tmp/t
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "CalTracker.h"

typedef CalTracker::Status S;
const uint32_t LINE = 2000, STEP = 100;

struct Sim {
  CalTracker t{4};
  uint32_t now = 0;
  float mean = 90.0f;
  bool probes = true;
  // Run ms with both channels in the given state; returns the last line's status.
  S run(uint32_t ms, bool open = false, bool retOnly = false) {
    S s = S::OUT;
    for (uint32_t e = 0; e < ms; e += STEP) {
      now += STEP;
      t.sample(now, open || retOnly, open);
      if (now % LINE == 0) s = t.line(now, mean, probes);
    }
    return s;
  }
};
const uint32_t SOAKED = 60000 + CalTracker::SOAK_MS + LINE;

int main() {
  { // The bath must hold flat 1 min + 5 min before a plugged pair logs, then 2 min finishes it.
    Sim s;
    assert(s.run(14000) == S::SETTLE);
    assert(s.run(20000) == S::SOAK);
    assert(s.run(SOAKED - 34000 - 2 * LINE) == S::SOAK);
    assert(s.run(2 * LINE) == S::LOG);
    assert(s.t.currentPair() == 1);
    s.run(CalTracker::WANT_LINES * LINE - 3 * LINE);
    assert(s.t.linesLogged() == CalTracker::WANT_LINES - 1);
    assert(s.run(LINE) == S::DONE);
    assert(s.run(10 * LINE) == S::DONE);

    // Swap: out 3 s, in; pair advances, settles 15 s with no new soak, logs again.
    assert(s.run(3000, true) == S::OUT);
    assert(s.t.currentPair() == 1);
    assert(s.run(LINE) == S::OUT);          // this window saw the plug open
    assert(s.t.currentPair() == 2);
    assert(s.run(LINE) == S::SETTLE);
    assert(s.run(CalTracker::SETTLE_MS) == S::LOG);

    // Four swaps wrap to pair 1.
    for (int want : {3, 4, 1}) {
      s.run(3000, true);
      s.run(2 * LINE);
      assert(s.t.currentPair() == want);
    }
  }
  { // A contact bounce under 1 s does not advance the pair, and its window is not counted.
    Sim s;
    s.run(SOAKED);
    assert(s.t.linesLogged() > 0);
    s.run(500, true);
    s.run(LINE);
    assert(s.t.currentPair() == 1);
    assert(s.run(LINE) == S::SETTLE);       // settling restarted from the bounce
  }
  { // One lead open: never logs, pair stays, even for a long time.
    Sim s;
    s.run(SOAKED);
    assert(s.run(20000, false, true) == S::OUT);
    assert(s.t.currentPair() == 1);
  }
  { // Boot with no plug: the first plug is pair 1.
    Sim s;
    s.run(5000, true);
    s.run(2 * LINE);
    assert(s.t.currentPair() == 1);
  }
  { // The bath moves: the count restarts and the soak is required again, pair unchanged.
    Sim s;
    s.run(SOAKED + 20 * LINE);
    assert(s.t.linesLogged() > 0);
    s.mean = 130.0f;
    assert(s.run(LINE) == S::SOAK);
    assert(s.t.linesLogged() == 0);
    assert(s.run(30000) == S::SOAK);        // the old value is still in the window
    assert(s.run(60000 + CalTracker::SOAK_MS) == S::LOG);
  }
  { // A probe that stops reading halts logging; the plug state still shows.
    Sim s;
    s.run(SOAKED + 2 * LINE);
    s.probes = false;
    assert(s.run(LINE) == S::NO_PROBE);
    assert(s.t.linesLogged() == 0);
    s.probes = true;
    assert(s.run(LINE) == S::LOG);          // the flat window never filled with anything else
  }
  { // setPair overrides, rejects out of range.
    Sim s;
    s.t.setPair(3);
    assert(s.t.currentPair() == 3);
    s.t.setPair(9);
    assert(s.t.currentPair() == 3);
  }
  puts("ok");
}
