// ScbSetJob.h: one read or write of the SCB's settings, in the order scripts/unico-scb.py proved on 2026-10-24.
//
// Every frame sent to the SCB is a write request, so the job answers only while it runs and then stops. It sends the idle
// frame until the SCB returns a 51-byte frame, the only one that reports every setting, and merges the requested changes
// into those stored values. It then sends one answer and watches for the changes to be reported as stored, repeating the
// same answer at most twice. It refuses to start, or stops, when another PC has answered in the last 10 s.
//
// Plain C++: the caller supplies the clock and the send function, so test/ can run it against a simulated SCB.

#pragma once

#include "ScbParser.h"
#include "ScbSettings.h"

class ScbSetJob {
 public:
  enum State { IDLE, ELICIT, VERIFY, DONE, FAILED };
  typedef void (*SendFn)(const uint8_t *frame, size_t len);

  static const size_t MAX_CHANGES = 12, KEY_LEN = 16, VAL_LEN = 12;

  State       state = IDLE;
  const char *result = "none";
  ScbSettings settings;               // the latest stored values
  bool        haveSettings = false;
  uint32_t    settingsMs = 0;

  explicit ScbSetJob(SendFn s) : send(s) {}

  bool busy() const { return state == ELICIT || state == VERIFY; }

  // clear(), then add() once per key=value. add() is false for an unknown key or a malformed value.
  void clear() { nChanges = 0; }
  bool add(const char *key, const char *val) {
    ScbSettings probe;
    if (nChanges >= MAX_CHANGES || strlen(key) >= KEY_LEN || strlen(val) >= VAL_LEN) return false;
    if (!scb::applyChange(probe, key, val)) return false;
    strcpy(keys[nChanges], key);
    strcpy(vals[nChanges], val);
    nChanges++;
    return true;
  }

  // Starts the job with the changes added since clear(); none means read the settings only. False when it cannot start.
  bool begin(uint32_t now, bool scbFresh) {
    if (busy()) return false;
    if (!scbFresh) return fail("no status frames from the SCB");
    if (foreignSeen && now - foreignMs < PC_QUIET_MS) return fail("another PC answered the SCB in the last 10 s");
    state = ELICIT;
    result = "working";
    deadline = now + ELICIT_MS;
    nextSend = now;
    sends = 0;
    return true;
  }

  // Call after the parser has completed status frames. Frames are only ever sent here, straight after the SCB's own.
  void onFrame(uint32_t now, const ScbParser &p) {
    notePc(now, p);
    if (p.lastLen == 51) {
      ScbSettings s;
      if (scb::stored(p.last, 51, s)) {
        settings = s;
        haveSettings = true;
        settingsMs = now;
      }
    }
    if (!busy()) return;
    if (foreignSeen && now - foreignMs < PC_QUIET_MS) { fail("another PC is answering the SCB"); return; }
    if (state == ELICIT) elicit(now, p);
    else if (state == VERIFY) verify(now, p);
  }

  // Call often: ends a job when its frames have stopped coming.
  void tick(uint32_t now, const ScbParser &p) {
    notePc(now, p);
    if (busy() && (int32_t)(now - deadline) > 0) {
      fail(state == ELICIT ? "no 51-byte frame from the SCB" : "the SCB did not report the new values as stored");
    }
  }

 private:
  static const uint32_t ELICIT_MS = 15000, VERIFY_MS = 12000, RESEND_MS = 3000, IDLE_EVERY_MS = 3000;
  static const uint32_t PC_QUIET_MS = 10000, ECHO_MS = 2000;
  static const int      MAX_SENDS = 3;

  SendFn      send;
  char        keys[MAX_CHANGES][KEY_LEN], vals[MAX_CHANGES][VAL_LEN];
  size_t      nChanges = 0;
  ScbSettings want;
  uint8_t     longFrame[51];
  uint32_t    deadline = 0, nextSend = 0, lastTx = 0, foreignMs = 0, seenPc = 0;
  bool        txEver = false, foreignSeen = false;
  int         sends = 0;

  bool fail(const char *why) {
    state = FAILED;
    result = why;
    return false;
  }
  void finish(const char *what) {
    state = DONE;
    result = what;
  }
  void transmit(const uint8_t *f, size_t n, uint32_t now) {
    send(f, n);
    lastTx = now;
    txEver = true;
  }
  // A PC frame soon after our own is our echo on the shared line; any other is another PC.
  void notePc(uint32_t now, const ScbParser &p) {
    if (p.nPc != seenPc) {
      seenPc = p.nPc;
      if (!txEver || now - lastTx > ECHO_MS) {
        foreignSeen = true;
        foreignMs = now;
      }
    }
  }

  void elicit(uint32_t now, const ScbParser &p) {
    if (p.lastLen != 51) {
      if (p.last[3] == 23 && (int32_t)(now - nextSend) >= 0) {
        uint8_t o[43];
        scb::idleFrame(p.last, o);
        transmit(o, sizeof(o), now);
        nextSend = now + IDLE_EVERY_MS;
      }
      return;
    }
    ScbSettings s;
    if (!scb::stored(p.last, 51, s)) { fail("the SCB does not report its Air Cycle mode; refusing to guess"); return; }
    if (nChanges == 0) { finish("read"); return; }
    want = s;
    for (size_t i = 0; i < nChanges; i++) scb::applyChange(want, keys[i], vals[i]);
    const char *bad = scb::checkLimits(want, p.last);
    if (bad) { fail(bad); return; }
    if (want == s) { finish("no change"); return; }
    memcpy(longFrame, p.last, 51);
    state = VERIFY;
    deadline = now + VERIFY_MS;
    sendAnswer(now);
  }

  void sendAnswer(uint32_t now) {
    uint8_t o[43];
    scb::answerFrame(longFrame, want, o);
    transmit(o, sizeof(o), now);
    sends++;
    nextSend = now + RESEND_MS;
  }

  void verify(uint32_t now, const ScbParser &p) {
    if (p.lastLen == 51) {
      ScbSettings s;
      if (scb::stored(p.last, 51, s) && s == want) { finish("stored"); return; }
    }
    if (sends < MAX_SENDS && (int32_t)(now - nextSend) >= 0) sendAnswer(now);
  }
};
