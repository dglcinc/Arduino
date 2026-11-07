// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// Host test of UnitCore.h and HttpRequest.h. "vectors" prints values for check_unit.py to compare with Python's hashlib
// and zlib; with no argument it runs the asserts.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "UnitCore.h"
#include "HttpRequest.h"
#include "ScbSettings.h"
using namespace unit;

static HttpRequest parse(const char *raw) {
  HttpRequest r;
  bool done = false;
  for (const char *p = raw; *p; p++) done = r.feed(*p);
  assert(done);
  return r;
}

static int vectors() {
  uint8_t d[32];
  char hex[65];
  for (const char *s : {"", "abc", "AirMaster sixty-four bytes of text fills one SHA block exactly!!", "a longer message that runs well past one sixty-four byte block of the compression function"}) {
    Sha256 h;
    h.add(s, strlen(s));
    h.finish(d);
    toHex(d, 32, hex);
    printf("sha256 %s %s\n", hex, s);
  }
  hashPassword("hunter2 pw", "00112233445566778899aabbccddeeff", hex);
  printf("pw %s\n", hex);
  printf("crc %08x\n", crc32((const uint8_t *)"123456789", 9));
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "vectors")) return vectors();

  { // identity: good, bad CRC, wrong magic, short password
    uint8_t p[256] = {0};
    memcpy(p, "AMID", 4);
    p[4] = 1;
    strcpy((char *)p + 8, "AM-0001");
    strcpy((char *)p + 24, "4F2A");
    strcpy((char *)p + 32, "abcd-efgh-jkmn");
    uint32_t c = crc32(p, 56);
    for (int i = 0; i < 4; i++) p[56 + i] = c >> (8 * i);
    Identity id = parseIdentity(p);
    assert(id.valid && !strcmp(id.serial, "AM-0001") && !strcmp(id.unitId, "4F2A") && !strcmp(id.setupPass, "abcd-efgh-jkmn"));
    p[33] ^= 1;
    assert(!parseIdentity(p).valid);
    p[33] ^= 1;
    p[0] = 'X';
    assert(!parseIdentity(p).valid);
    uint8_t erased[256];
    memset(erased, 0xFF, 256);
    assert(!parseIdentity(erased).valid);
  }
  { // config round trip, with characters a text file would trip on
    UnitConfig c;
    strcpy(c.ssid, "Joe's \"Wi=Fi\"\n#2");
    strcpy(c.pass, "p&ss=w0rd%");
    strcpy(c.name, "Master Bedroom");
    strcpy(c.salt, "00112233445566778899aabbccddeeff");
    hashPassword("x", c.salt, c.hash);
    c.romSup[0] = 0x28; c.romSup[7] = 0xAB;
    c.offAret = -0.25f;
    strcpy(c.mqttHost, "broker.local");
    strcpy(c.mqttUser, "ha user");
    strcpy(c.mqttPass, "p&ss=%");
    c.mqttPort = 1884;
    c.mqttWrites = true;
    char b[1500];
    size_t n = saveConfig(c, b, sizeof(b));
    assert(n < sizeof(b));
    UnitConfig o;
    assert(loadConfig(b, o));
    assert(!strcmp(o.ssid, c.ssid) && !strcmp(o.pass, c.pass) && !strcmp(o.name, c.name) && !strcmp(o.hash, c.hash));
    assert(o.romSup[0] == 0x28 && o.romSup[7] == 0xAB && o.romRet[0] == 0);
    assert(o.offAret == -0.25f && o.calibrated() && o.hasWifi() && o.hasAdmin());
    assert(!strcmp(o.mqttHost, "broker.local") && !strcmp(o.mqttUser, "ha user") && !strcmp(o.mqttPass, "p&ss=%") && o.mqttPort == 1884 && o.mqttWrites);
    char oldFile[] = "v=1\nssid=4142\n";            // a file saved before MQTT existed
    UnitConfig older;
    assert(loadConfig(oldFile, older) && !strcmp(older.ssid, "AB") && !older.mqttHost[0] && older.mqttPort == 1883 && !older.mqttWrites);
    char junk[] = "this is not a config";
    UnitConfig untouched;
    assert(!loadConfig(junk, untouched) && !untouched.hasWifi());
    char noVersion[] = "ssid=4142\n";
    assert(!loadConfig(noVersion, untouched));
    char badHex[] = "v=1\nssid=zz\n";
    assert(!loadConfig(badHex, untouched));
    char maxed[1500];
    UnitConfig m;
    memset(m.ssid, 'a', 32);
    memset(m.pass, 'b', 63);
    memset(m.name, 'c', 32);
    memset(m.mqttHost, 'd', 63);
    memset(m.mqttUser, 'e', 32);
    memset(m.mqttPass, 'f', 63);
    assert(saveConfig(m, maxed, sizeof(maxed)) < sizeof(maxed));
  }
  { // forms
    char out[33];
    const char *f = "ssid=Joe%27s+Net&pass=a%26b&empty=&name=M";
    assert(formGet(f, "ssid", out, sizeof(out)) && !strcmp(out, "Joe's Net"));
    assert(formGet(f, "pass", out, sizeof(out)) && !strcmp(out, "a&b"));
    assert(formGet(f, "empty", out, sizeof(out)) && !out[0]);
    assert(formGet(f, "name", out, sizeof(out)) && !strcmp(out, "M"));
    assert(!formGet(f, "nope", out, sizeof(out)));
    assert(!formGet(f, "ss", out, sizeof(out)));
    char tiny[3];
    assert(!formGet(f, "ssid", tiny, sizeof(tiny)));
  }
  { // host names
    char h[48];
    hostName("Master Bedroom", "4F2A", h, sizeof(h));
    assert(!strcmp(h, "master-bedroom"));
    hostName("  Kid's Room #2!", "4F2A", h, sizeof(h));
    assert(!strcmp(h, "kid-s-room-2"));
    hostName("", "4F2A", h, sizeof(h));
    assert(!strcmp(h, "airmaster-4f2a"));
    hostName("!!!", "4F2A", h, sizeof(h));
    assert(!strcmp(h, "airmaster-4f2a"));
  }
  { // JSON escaping, and it stays inside the buffer
    char b[64];
    size_t n = jsonStr(b, 0, sizeof(b), "a\"b\\c\nd");
    assert(!strcmp(b, "a\\\"b\\\\c\\u000ad") && n == strlen(b));
    char small[10];
    n = jsonStr(small, 0, sizeof(small), "0123456789012345");
    assert(n < sizeof(small));
  }
  { // requests
    HttpRequest r = parse("GET /settings?a=1 HTTP/1.1\r\nHost: airmaster.local\r\nAccept: text/html,*/*\r\nCookie: x=1; s=abc123; y=2\r\n\r\n");
    assert(!strcmp(r.method, "GET") && !strcmp(r.target, "/settings") && !strcmp(r.query(), "a=1"));
    assert(!strcmp(r.host, "airmaster.local") && r.wantsHtml && !strcmp(r.session, "abc123") && !r.markedApi);
    HttpRequest p = parse("POST /api/set HTTP/1.1\r\ncontent-length: 12\r\nx-airmaster: 1\r\n\r\n");
    assert(!strcmp(p.method, "POST") && p.contentLength == 12 && p.markedApi && !p.wantsHtml && !p.query());
    HttpRequest j = parse("GET / HTTP/1.1\r\nAccept: */*\r\nUser-Agent: python-requests/2\r\n\r\n");
    assert(!j.wantsHtml);
    HttpRequest big;
    for (int i = 0; i < 1000; i++) big.feed('a');
    assert(big.tooLong);
    assert(!big.feed('\n'));
  }
  { // a blank line before any request line does not end the request
    HttpRequest r;
    assert(!r.feed('\n'));
    assert(!r.sawAnything());
  }
  { // the saved originals: every setting survives the trip through the form, at the longest it can be
    ScbSettings s;
    for (int k = 0; k < 6; k++) { s.cfm[k] = 1500 - k; s.rpm[k] = 1800 - k; }
    s.percent = 255; s.blower = 1500; s.sqft = 30000; s.occupants = 51; s.runmin = 60; s.interval = 24; s.bedrooms = 50;
    s.eac = true; s.autoMode = true; s.energySaver = true; s.powerOpen = true;
    char form[420];
    size_t n = scb::settingsForm(s, form, sizeof(form));
    assert(n < sizeof(form));
    ScbSettings o;
    int pairs = 0;
    for (char *p = form, *next; p && *p; p = next, pairs++) {
      next = strchr(p, '&');
      if (next) *next++ = 0;
      char *eq = strchr(p, '=');
      *eq = 0;
      assert(strlen(p) < 16 && strlen(eq + 1) < 12);   // ScbSetJob's KEY_LEN and VAL_LEN
      assert(scb::applyChange(o, p, eq + 1));
    }
    assert(o == s && pairs <= 24);                     // ScbSetJob's MAX_CHANGES
  }
  puts("ok");
}
