// UnitCore.h: what makes a board one particular AirMaster, with no hardware in it, so test/unit_test.cpp runs it on the
// Mac: the factory identity, the saved configuration, password hashing and the small text helpers the web side needs.
//
// The factory tool (AirMaster/factory) writes the identity into one 256-byte page at the start of the flash's last
// sector, which no firmware update or filesystem format touches:
//   0 "AMID", 4 version 1, 8 serial[16], 24 unit id[8], 32 setup password[24], 56 CRC-32 of bytes 0-55, little-endian
// Text fields are NUL-terminated. A change here needs the same change in factory/airmaster_factory.py.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

namespace unit {

// --- SHA-256 --------------------------------------------------------------------------------------

inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

class Sha256 {
 public:
  Sha256() {
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(h, H0, sizeof(h));
  }
  void add(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    total += len;
    while (len--) {
      block[used++] = *p++;
      if (used == 64) { compress(); used = 0; }
    }
  }
  void finish(uint8_t out[32]) {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    add(&pad, 1);
    pad = 0;
    while (used != 56) add(&pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    add(len, 8);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
  }
 private:
  uint32_t h[8];
  uint8_t  block[64];
  size_t   used = 0;
  uint64_t total = 0;
  void compress() {
    static const uint32_t K[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
      0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
      0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
      0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = ((uint32_t)block[4*i] << 24) | ((uint32_t)block[4*i+1] << 16) | ((uint32_t)block[4*i+2] << 8) | block[4*i+3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (ror(e,6)^ror(e,11)^ror(e,25)) + ((e&f)^(~e&g)) + K[i] + w[i];
      uint32_t t2 = (ror(a,2)^ror(a,13)^ror(a,22)) + ((a&b)^(a&c)^(b&c));
      hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
  }
};

inline void toHex(const uint8_t *b, size_t n, char *out) {
  static const char D[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2*i] = D[b[i] >> 4]; out[2*i+1] = D[b[i] & 15]; }
  out[2*n] = 0;
}

inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes hex into out, at most outLen bytes. The number of bytes, or -1 when the text is not hex or too long.
inline int fromHex(const char *h, uint8_t *out, size_t outLen) {
  size_t n = strlen(h);
  if (n % 2 || n / 2 > outLen) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    int a = hexVal(h[2*i]), b = hexVal(h[2*i+1]);
    if (a < 0 || b < 0) return -1;
    out[i] = (uint8_t)(a << 4 | b);
  }
  return (int)(n / 2);
}

// --- passwords ------------------------------------------------------------------------------------

const int HASH_ROUNDS = 5000;       // well under 1 s on the RP2040 and slows a guess at a stolen file
const size_t SALT_HEX = 32, HASH_HEX = 64;

inline void hashPassword(const char *pw, const char *saltHex, char out[HASH_HEX + 1]) {
  uint8_t d[32];
  Sha256 first;
  first.add(saltHex, strlen(saltHex));
  first.add(pw, strlen(pw));
  first.finish(d);
  for (int i = 0; i < HASH_ROUNDS; i++) {
    Sha256 s;
    s.add(d, 32);
    s.finish(d);
  }
  toHex(d, 32, out);
}

// Compares every byte, so the time taken says nothing about where two values differ.
inline bool sameText(const char *a, const char *b) {
  size_t na = strlen(a), nb = strlen(b);
  if (na != nb) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < na; i++) diff |= a[i] ^ b[i];
  return diff == 0;
}

// --- the factory identity -------------------------------------------------------------------------

struct Identity {
  bool valid = false;
  char serial[16] = "";
  char unitId[8] = "";
  char setupPass[24] = "";
};

inline uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
  }
  return ~c;
}

inline bool terminated(const uint8_t *p, size_t n) { return memchr(p, 0, n) != nullptr; }

inline Identity parseIdentity(const uint8_t *p) {
  Identity id;
  if (memcmp(p, "AMID", 4) || p[4] != 1) return id;
  uint32_t want = p[56] | (uint32_t)p[57] << 8 | (uint32_t)p[58] << 16 | (uint32_t)p[59] << 24;
  if (crc32(p, 56) != want) return id;
  if (!terminated(p + 8, 16) || !terminated(p + 24, 8) || !terminated(p + 32, 24)) return id;
  strcpy(id.serial, (const char *)p + 8);
  strcpy(id.unitId, (const char *)p + 24);
  strcpy(id.setupPass, (const char *)p + 32);
  id.valid = strlen(id.setupPass) >= 8 && id.unitId[0] && id.serial[0];   // WPA2 needs 8
  return id;
}

// --- text helpers ---------------------------------------------------------------------------------

// Decodes a form value in place: + to space, %XX to its byte.
inline void urlDecode(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '+') { *o++ = ' '; }
    else if (*s == '%' && hexVal(s[1]) >= 0 && hexVal(s[2]) >= 0) { *o++ = (char)(hexVal(s[1]) << 4 | hexVal(s[2])); s += 2; }
    else { *o++ = *s; }
  }
  *o = 0;
}

// Finds key in "a=b&c=d", copies its decoded value to out and returns true. A value that does not fit is refused.
inline bool formGet(const char *form, const char *key, char *out, size_t outLen) {
  size_t kl = strlen(key);
  for (const char *p = form; p && *p;) {
    const char *amp = strchr(p, '&');
    size_t len = amp ? (size_t)(amp - p) : strlen(p);
    if (len > kl && p[kl] == '=' && !strncmp(p, key, kl)) {
      size_t vl = len - kl - 1;
      if (vl >= outLen * 3) return false;
      char buf[256];
      if (vl >= sizeof(buf)) return false;
      memcpy(buf, p + kl + 1, vl);
      buf[vl] = 0;
      urlDecode(buf);
      if (strlen(buf) >= outLen) return false;
      strcpy(out, buf);
      return true;
    }
    p = amp ? amp + 1 : nullptr;
  }
  return false;
}

// A DNS label from the unit's name: "Master Bedroom" -> master-bedroom. Empty or all punctuation falls back to
// airmaster-<unit id>.
inline void hostName(const char *name, const char *unitId, char *out, size_t outLen) {
  size_t n = 0;
  bool dash = false;
  for (const char *p = name; *p && n + 2 < outLen && n < 40; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) && c < 128) {
      if (dash && n) out[n++] = '-';
      out[n++] = (char)tolower(c);
      dash = false;
    } else {
      dash = true;
    }
  }
  out[n] = 0;
  if (!n) {
    snprintf(out, outLen, "airmaster-%s", unitId);
    for (char *q = out; *q; q++) *q = (char)tolower((unsigned char)*q);
  }
}

// Appends s as the inside of a JSON string. Returns the new length, never past len - 1.
inline size_t jsonStr(char *b, size_t n, size_t len, const char *s) {
  for (; *s && n + 7 < len; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { b[n++] = '\\'; b[n++] = (char)c; }
    else if (c < 0x20) { n += snprintf(b + n, len - n, "\\u%04x", c); }
    else { b[n++] = (char)c; }
  }
  if (n < len) b[n] = 0;
  return n;
}

// --- the saved configuration ----------------------------------------------------------------------

struct UnitConfig {
  char  ssid[33] = "";
  char  pass[64] = "";
  char  name[33] = "";
  char  salt[SALT_HEX + 1] = "";
  char  hash[HASH_HEX + 1] = "";
  uint8_t romSup[8] = {0}, romRet[8] = {0};   // zero: the first two probes in search order
  float offWsup = 0, offWret = 0, offAsup = 0, offAret = 0;   // degrees F, added to the reading
  char  mqttHost[64] = "";                    // empty: MQTT off
  char  mqttUser[33] = "";
  char  mqttPass[64] = "";
  unsigned mqttPort = 1883;
  bool  mqttWrites = false;                   // Home Assistant may change stored airflow; the owner opts in

  bool hasWifi() const { return ssid[0] != 0; }
  bool hasAdmin() const { return hash[0] != 0; }
  bool calibrated() const { return offWsup != 0 || offWret != 0 || offAsup != 0 || offAret != 0; }
};

// One key=value line each. SSID, WiFi password and unit name are hex, since they can hold any character.
inline size_t saveConfig(const UnitConfig &c, char *b, size_t len) {
  char ssid[67], pass[129], name[67], ra[17], rb[17], mh[129], mu[67], mp[129];
  toHex((const uint8_t *)c.mqttHost, strlen(c.mqttHost), mh);
  toHex((const uint8_t *)c.mqttUser, strlen(c.mqttUser), mu);
  toHex((const uint8_t *)c.mqttPass, strlen(c.mqttPass), mp);
  toHex((const uint8_t *)c.ssid, strlen(c.ssid), ssid);
  toHex((const uint8_t *)c.pass, strlen(c.pass), pass);
  toHex((const uint8_t *)c.name, strlen(c.name), name);
  toHex(c.romSup, 8, ra);
  toHex(c.romRet, 8, rb);
  return snprintf(b, len, "v=1\nssid=%s\npass=%s\nname=%s\nsalt=%s\nhash=%s\nromsup=%s\nromret=%s\noffwsup=%.3f\noffwret=%.3f\noffasup=%.3f\noffaret=%.3f\nmh=%s\nmu=%s\nmp=%s\nmport=%u\nmw=%d\n",
                  ssid, pass, name, c.salt, c.hash, ra, rb, c.offWsup, c.offWret, c.offAsup, c.offAret, mh, mu, mp, c.mqttPort, c.mqttWrites ? 1 : 0);
}

inline bool hexField(const char *v, char *out, size_t outLen) {
  uint8_t tmp[64];
  if (outLen - 1 > sizeof(tmp)) return false;
  int n = fromHex(v, tmp, outLen - 1);
  if (n < 0) return false;
  memcpy(out, tmp, n);
  out[n] = 0;
  return true;
}

// Text is modified in place. False when the file is not one this firmware wrote; c is then left as defaults.
inline bool loadConfig(char *text, UnitConfig &out) {
  UnitConfig c;
  bool sawVersion = false;
  for (char *line = text, *next; line && *line; line = next) {
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    char *eq = strchr(line, '=');
    if (!eq) return false;
    *eq = 0;
    const char *k = line, *v = eq + 1;
    bool ok = true;
    if (!strcmp(k, "v")) { sawVersion = !strcmp(v, "1"); }
    else if (!strcmp(k, "ssid")) ok = hexField(v, c.ssid, sizeof(c.ssid));
    else if (!strcmp(k, "pass")) ok = hexField(v, c.pass, sizeof(c.pass));
    else if (!strcmp(k, "name")) ok = hexField(v, c.name, sizeof(c.name));
    else if (!strcmp(k, "salt")) { ok = strlen(v) <= SALT_HEX; if (ok) strcpy(c.salt, v); }
    else if (!strcmp(k, "hash")) { ok = strlen(v) <= HASH_HEX; if (ok) strcpy(c.hash, v); }
    else if (!strcmp(k, "romsup")) ok = fromHex(v, c.romSup, 8) == 8;
    else if (!strcmp(k, "romret")) ok = fromHex(v, c.romRet, 8) == 8;
    else if (!strcmp(k, "offwsup")) c.offWsup = strtof(v, nullptr);
    else if (!strcmp(k, "offwret")) c.offWret = strtof(v, nullptr);
    else if (!strcmp(k, "offasup")) c.offAsup = strtof(v, nullptr);
    else if (!strcmp(k, "offaret")) c.offAret = strtof(v, nullptr);
    else if (!strcmp(k, "mh")) ok = hexField(v, c.mqttHost, sizeof(c.mqttHost));
    else if (!strcmp(k, "mu")) ok = hexField(v, c.mqttUser, sizeof(c.mqttUser));
    else if (!strcmp(k, "mp")) ok = hexField(v, c.mqttPass, sizeof(c.mqttPass));
    else if (!strcmp(k, "mport")) c.mqttPort = (unsigned)strtoul(v, nullptr, 10);
    else if (!strcmp(k, "mw")) c.mqttWrites = v[0] == '1';
    if (!ok) return false;
  }
  if (!sawVersion) return false;
  out = c;
  return true;
}

}  // namespace unit
