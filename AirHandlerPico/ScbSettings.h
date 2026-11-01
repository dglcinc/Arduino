// ScbSettings.h: the SCB's stored settings and the frames that request them, ported from pivac's
// pivac/unico_scb_protocol.py (stored_settings, answer_frame, idle_frame) and the limits and key names of
// scripts/unico-scb.py (check_limits, parse_changes). Plain C++, so test/ can compare it with the Python.
//
// This file has no way to send the thermostat override or the undocumented test bytes: answerFrame() leaves byte 4's
// override bits clear and bytes 39 and 40 zero. pivac docs/unico-scb-protocol.md §8 says why.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

struct ScbSettings {
  uint16_t cfm[6] = {0}, rpm[6] = {0};   // G Y1 Y2 W1 W2 E
  uint8_t  percent = 0, occupants = 0, runmin = 0, interval = 0, bedrooms = 0;
  uint16_t blower = 0, sqft = 0;
  bool     eac = false, autoMode = false, energySaver = false, powerOpen = false;

  bool operator==(const ScbSettings &o) const {
    return !memcmp(cfm, o.cfm, sizeof(cfm)) && !memcmp(rpm, o.rpm, sizeof(rpm)) && percent == o.percent &&
           occupants == o.occupants && runmin == o.runmin && interval == o.interval && bedrooms == o.bedrooms &&
           blower == o.blower && sqft == o.sqft && eac == o.eac && autoMode == o.autoMode &&
           energySaver == o.energySaver && powerOpen == o.powerOpen;
  }
};

namespace scb {

const uint8_t  STATUS_ID = 0x83, PC_TAG = 0x1C;
const uint16_t RPM_MAX = 1800;
static const char *const MODE_KEYS[6] = {"G", "Y1", "Y2", "W1", "W2", "E"};

inline uint16_t u16(const uint8_t *f, size_t i) { return (uint16_t(f[i]) << 8) | f[i + 1]; }

// Appends the two Fletcher check bytes the SCB expects after n body bytes.
inline void withCheck(uint8_t *f, size_t n) {
  uint16_t s1 = 0, s2 = 0;
  for (size_t i = 0; i < n; i++) {
    s1 = (s1 + f[i]) % 255;
    s2 = (s2 + s1) % 255;
  }
  uint8_t c0 = 255 - ((s1 + s2) % 255);
  uint8_t c1 = 255 - ((s1 + c0) % 255);
  f[n] = c0;
  f[n + 1] = c1;
}

// False unless f is a 51-byte frame with byte 3 = 23 and a known Air Cycle mode (SCB byte 2 bit 7).
inline bool stored(const uint8_t *f, size_t len, ScbSettings &s) {
  if (len != 51 || f[3] != 23 || !(f[2] & 0x80)) return false;
  for (int k = 0; k < 6; k++) {
    s.cfm[k] = u16(f, 6 + 2 * k);
    s.rpm[k] = u16(f, 18 + 2 * k);
  }
  uint16_t sw = u16(f, 38);
  s.percent = f[34];
  s.blower = u16(f, 35);
  s.sqft = u16(f, 40);
  s.occupants = f[42] & 0x7F;
  s.runmin = f[43];
  s.interval = f[44];
  s.bedrooms = f[45] & 0x3F;
  s.eac = f[45] & 0x40;
  s.autoMode = f[45] & 0x80;
  s.energySaver = sw & 0x2000;
  s.powerOpen = sw & 0x4000;
  return true;
}

inline uint16_t cfmCap(const uint8_t *f) {
  static const uint16_t CAP[4] = {500, 750, 1200, 1500};   // 1218 2430 3642 4860
  if (f[5] == 12 && (u16(f, 38) & 0x0400)) return 1200;    // the 3036
  return CAP[f[4] & 3];
}

inline uint16_t rpmFloor(const uint8_t *f) {
  return f[5] == 12 && (u16(f, 38) & 0x0400) ? 600 : 400;  // the 3036
}

// check_limits(): null when every value is in range, else what is not.
inline const char *checkLimits(const ScbSettings &s, const uint8_t *f) {
  uint16_t cap = cfmCap(f), floor = rpmFloor(f);
  for (int k = 0; k < 6; k++) {
    if (s.cfm[k] == 0 || s.cfm[k] > cap) return "airflow outside this model's range";
    if (s.rpm[k] < floor || s.rpm[k] > RPM_MAX) return "RPM limit outside this model's range";
  }
  if (s.blower > cap) return "air.blower over this model's limit";
  if (s.sqft > 30000) return "air.sqft over 30000";
  if (s.occupants > 51) return "air.occupants over 51";
  if (s.runmin > 60) return "air.runmin over 60";
  if (s.interval > 24) return "air.interval over 24";
  if (s.bedrooms > 50) return "air.bedrooms over 50";
  return nullptr;
}

// answer_frame(): the 43-byte frame that requests want from the SCB whose 51-byte frame is f.
inline void answerFrame(const uint8_t *f, const ScbSettings &w, uint8_t *o) {
  size_t n = 0;
  o[n++] = 0x01;
  o[n++] = STATUS_ID;
  o[n++] = (f[2] & 1) | (w.energySaver ? 2 : 0) | (w.powerOpen ? 4 : 0);
  o[n++] = PC_TAG;
  o[n++] = (w.autoMode ? 2 : 0) | (w.eac ? 1 : 0);
  for (int k = 0; k < 6; k++) { o[n++] = w.cfm[k] >> 8; o[n++] = w.cfm[k] & 0xFF; }
  for (int k = 0; k < 6; k++) { o[n++] = w.rpm[k] >> 8; o[n++] = w.rpm[k] & 0xFF; }
  o[n++] = w.percent;
  o[n++] = w.blower >> 8;
  o[n++] = w.blower & 0xFF;
  o[n++] = 0;                            // byte 32, the diagnostic windows
  o[n++] = w.sqft >> 8;
  o[n++] = w.sqft & 0xFF;
  o[n++] = w.occupants;
  o[n++] = w.runmin;
  o[n++] = w.interval;
  o[n++] = (w.bedrooms & 0x3F) | (w.eac ? 0x40 : 0) | (w.autoMode ? 0x80 : 0);
  o[n++] = 0;                            // test flow, never set
  o[n++] = 0;                            // test pressure, never set
  withCheck(o, n);
}

// idle_frame(): requests nothing, and brings the 51-byte frame on the next tick.
inline void idleFrame(const uint8_t *f, uint8_t *o) {
  memset(o, 0, 41);
  o[0] = 0x01;
  o[1] = STATUS_ID;
  o[2] = (f[2] & 1) | 0xFE;
  o[3] = PC_TAG;
  o[4] = 0x01;
  withCheck(o, 41);
}

inline bool ieq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return !*a && !*b;
}

inline bool parseUint(const char *v, long maxV, long &out) {
  if (!*v) return false;
  char *end;
  long n = strtol(v, &end, 10);
  if (*end || n < 0 || n > maxV) return false;
  out = n;
  return true;
}

// parse_changes() for one key=value, in the Mac tool's names: cfm.Y2=500 rpm.G=900 damper=poweropen auto.style=energysaver
// air.percent air.blower air.sqft air.occupants air.runmin air.interval air.bedrooms air.eac=on air.mode=auto.
// False for an unknown key or a malformed value. Ranges that depend on the model wait for checkLimits().
inline bool applyChange(ScbSettings &s, const char *key, const char *val) {
  long n;
  bool isCfm = !strncmp(key, "cfm.", 4), isRpm = !strncmp(key, "rpm.", 4);
  if (isCfm || isRpm) {
    for (int k = 0; k < 6; k++) {
      if (ieq(key + 4, MODE_KEYS[k])) {
        if (!parseUint(val, 65535, n)) return false;
        (isCfm ? s.cfm : s.rpm)[k] = (uint16_t)n;
        return true;
      }
    }
    return false;
  }
  if (!strcmp(key, "damper")) {
    if (!strcmp(val, "poweropen")) s.powerOpen = true;
    else if (!strcmp(val, "powerclose")) s.powerOpen = false;
    else return false;
    return true;
  }
  if (!strcmp(key, "auto.style")) {
    if (!strcmp(val, "energysaver")) s.energySaver = true;
    else if (!strcmp(val, "maxcomfort")) s.energySaver = false;
    else return false;
    return true;
  }
  if (!strcmp(key, "air.eac")) {
    if (!strcmp(val, "on")) s.eac = true;
    else if (!strcmp(val, "off")) s.eac = false;
    else return false;
    return true;
  }
  if (!strcmp(key, "air.mode")) {
    if (!strcmp(val, "auto")) s.autoMode = true;
    else if (!strcmp(val, "fixed")) s.autoMode = false;
    else return false;
    return true;
  }
  struct { const char *k; long m; } lim[] = {{"air.percent", 255}, {"air.blower", 65535}, {"air.sqft", 30000},
    {"air.occupants", 51}, {"air.runmin", 60}, {"air.interval", 24}, {"air.bedrooms", 50}};
  for (auto &l : lim) {
    if (strcmp(key, l.k)) continue;
    if (!parseUint(val, l.m, n)) return false;
    if (!strcmp(key, "air.percent")) s.percent = n;
    else if (!strcmp(key, "air.blower")) s.blower = n;
    else if (!strcmp(key, "air.sqft")) s.sqft = n;
    else if (!strcmp(key, "air.occupants")) s.occupants = n;
    else if (!strcmp(key, "air.runmin")) s.runmin = n;
    else if (!strcmp(key, "air.interval")) s.interval = n;
    else s.bedrooms = n;
    return true;
  }
  return false;
}

}  // namespace scb
