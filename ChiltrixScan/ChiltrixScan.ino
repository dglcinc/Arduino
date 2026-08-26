/*
 * ChiltrixScan - interactive Modbus RTU register scanner for a Chiltrix CX
 *
 * Arduino UNO R4 (Minima or WiFi) + RS-485 shield, driven over USB from a
 * serial monitor at 115200.  No WiFi, no pivac.  Its only job is to find the
 * unit and identify which register is which.
 *
 * Why this exists: two community register maps disagree on nearly every
 * address.  jasipsw/homeassistant-chiltrix-modbus (CX50-2) calls 202 water
 * inlet; gonzojive/heatpump (CX34) calls it ambient.  Neither covers a CX75.
 * Only the unit settles it.
 *
 * Chiltrix's own ProtoAir gateway guide specifies Modbus RTU 9600/N/8/1 and
 * Node-ID 1 for a single chiller, so that is where the probe starts.
 *
 * READ ONLY.  Function code 3 exclusively.  Writing to a misidentified
 * register can move a setpoint or a protection limit.
 */

#include <ModbusMaster.h>

// ---- wiring -----------------------------------------------------------------
// DE and RE are normally tied together on the shield and driven by one pin.
// -1 means the shield directs itself.  If you do not know which yours is, the
// 'd' command tries every candidate for you.
int deP = -1;   // -1 = shield's DIP switch is on AUTO

#define MODBUS_SERIAL  Serial1  // UNO R4: D0 = RX, D1 = TX, independent of USB Serial

ModbusMaster node;

const uint32_t BAUDS[] = {9600, 19200, 4800, 38400, 57600, 115200, 2400};
const uint8_t  NBAUDS  = sizeof(BAUDS) / sizeof(BAUDS[0]);

uint32_t curBaud = 9600;
uint8_t  curId   = 1;

// ---- what the two maps claim, for the addresses that matter -----------------
struct Label { uint16_t addr; const char *cx50; const char *cx34; };
const Label LABELS[] = {
  {  53, "",                  "P53 EC pump min speed" },
  {  59, "",                  "P59 antifreeze?"       },
  {  65, "",                  "P65 min water flow?"   },
  { 109, "",                  "P109 target range?"    },
  { 140, "",                  "on/off"                },
  { 141, "",                  "AC mode 0=cool 1=heat" },
  { 142, "",                  "target cool temp"      },
  { 202, "water inlet T",     "ambient T"             },
  { 203, "water outlet T",    "suction T"             },
  { 204, "ambient T",         "plate HX T"            },
  { 205, "coil T",            "AC outlet water T"     },
  { 206, "discharge T",       "solar T"               },
  { 207, "suction T",         ""                      },
  { 209, "SETPOINT T",        "compressor current"    },
  { 213, "DHW setpoint",      "WATER FLOW RATE"       },
  { 214, "ANTIFREEZE T (P59)", "P03 status"           },
  { 225, "",                  "inner water flow sw"   },
  { 227, "",                  "compressor frequency"  },
  { 243, "operating state",   "indoor temp ctrl sw"   },
  { 244, "error code",        "fan type"              },
  { 248, "",                  "internal pump speed"   },
  { 256, "current power W",   "input AC current"      },
  { 257, "FLOW RATE 0.1L/min", "compressor phase I"   },
  { 258, "compressor speed %", "bus line voltage"     },
  { 259, "fan speed %",       "fan shutdown code"     },
  { 260, "pump speed %",      "IPM temp"              },
  { 261, "system pressure",   "compressor run hours"  },
  { 264, "run hours (u32)",   ""                      },
  { 281, "comp starts (u32)", "water inlet sensor 1"  },
  { 284, "",                  "current fault code"    },
  { 285, "defrost count",     ""                      },
};
const uint8_t NLABELS = sizeof(LABELS) / sizeof(LABELS[0]);

const Label *lookup(uint16_t a) {
  for (uint8_t i = 0; i < NLABELS; i++) if (LABELS[i].addr == a) return &LABELS[i];
  return NULL;
}

// ---- RS-485 direction -------------------------------------------------------
void preTx()  { if (deP >= 0) digitalWrite(deP, HIGH); }
void postTx() { if (deP >= 0) digitalWrite(deP, LOW);  }

void applyDe(int p) {
  if (deP >= 0) pinMode(deP, INPUT);       // release the old one
  deP = p;
  if (deP >= 0) { pinMode(deP, OUTPUT); digitalWrite(deP, LOW); }
}

bool linkUp = false;
void applyLink() {
  if (linkUp) MODBUS_SERIAL.end();   // calling end() before the first begin() bus-faults on the UNO R4
  MODBUS_SERIAL.begin(curBaud, SERIAL_8N1);
  linkUp = true;
  node.begin(curId, MODBUS_SERIAL);
  node.preTransmission(preTx);
  node.postTransmission(postTx);
}

const char *rcName(uint8_t rc) {
  switch (rc) {
    case 0x00: return "OK";
    case 0x01: return "EX01 illegal function";
    case 0x02: return "EX02 illegal address";
    case 0x03: return "EX03 illegal value";
    case 0x04: return "EX04 device failure";
    case 0xE0: return "invalid slave id";
    case 0xE1: return "invalid function";
    case 0xE2: return "timeout";
    case 0xE3: return "CRC ERROR";
    default:   return "?";
  }
}

// A response of ANY kind -- success or a Modbus exception -- proves a device is
// there at this baud and id.  Only a timeout means nothing answered.  A CRC
// error means something is transmitting but the baud rate is wrong.
bool answered(uint8_t rc) { return rc <= 0x04; }

// ---- probing ----------------------------------------------------------------
bool tryHere() {
  node.clearResponseBuffer();
  uint8_t rc = node.readHoldingRegisters(202, 1);
  if (answered(rc)) return true;
  if (rc == 0xE3) Serial.println(F("      (CRC errors -- something is talking, baud is probably wrong)"));
  return false;
}

void probe(uint8_t idFrom, uint8_t idTo) {
  Serial.println(F("probing..."));
  for (uint8_t b = 0; b < NBAUDS; b++) {
    for (uint16_t id = idFrom; id <= idTo; id++) {
      curBaud = BAUDS[b]; curId = id; applyLink();
      Serial.print(F("  ")); Serial.print(curBaud);
      Serial.print(F(" 8N1 id=")); Serial.print(id); Serial.print(F(" ... "));
      if (tryHere()) {
        Serial.println(F("RESPONDED"));
        Serial.print(F("\nfound: baud=")); Serial.print(curBaud);
        Serial.print(F(" id=")); Serial.println(curId);
        Serial.println(F("run 'k' next -- the known-value checks\n"));
        return;
      }
      Serial.println(F("no"));
    }
  }
  Serial.println(F("\nnothing answered. check: A/B swapped, GND connected,"));
  Serial.println(F("DE pin correct, and that you are on the BMS terminals.\n"));
  curBaud = 9600; curId = 1; applyLink();
}

// ---- reading ----------------------------------------------------------------
// Returns true and sets *out on success; prints the failure otherwise.
bool readOne(uint16_t addr, uint16_t *out) {
  node.clearResponseBuffer();
  uint8_t rc = node.readHoldingRegisters(addr, 1);
  if (rc == node.ku8MBSuccess) { *out = node.getResponseBuffer(0); return true; }
  return false;
}

void printRow(uint16_t addr, uint16_t raw) {
  int16_t  s = (int16_t)raw;
  float    c = s / 10.0f;
  Serial.print(addr); Serial.print(F("\t"));
  Serial.print(raw);  Serial.print(F("\t0x"));
  if (raw < 0x1000) Serial.print(F("0"));
  if (raw < 0x100)  Serial.print(F("0"));
  if (raw < 0x10)   Serial.print(F("0"));
  Serial.print(raw, HEX); Serial.print(F("\t"));
  Serial.print(s);    Serial.print(F("\t"));
  Serial.print(c, 1); Serial.print(F("C/"));
  Serial.print(c * 9.0f / 5.0f + 32.0f, 1); Serial.print(F("F\t"));
  Serial.print(raw / 10.0f, 1); Serial.print(F("L/m\t"));
  const Label *l = lookup(addr);
  if (l) {
    Serial.print(F("cx50:")); Serial.print(l->cx50);
    Serial.print(F("  cx34:")); Serial.print(l->cx34);
  }
  Serial.println();
}

void header() {
  Serial.println(F("\naddr\traw\thex\tint16\tx0.1 temp\tx0.1 flow\tcandidate meanings"));
  Serial.println(F("--------------------------------------------------------------------------"));
}

void scan(uint16_t from, uint16_t to) {
  Serial.print(F("scanning ")); Serial.print(from); Serial.print(F(" to ")); Serial.println(to);
  Serial.println(F("(EX02 = the address does not exist; that is useful, not an error)"));
  header();
  uint16_t hits = 0, v;
  for (uint16_t a = from; a <= to; a++) {
    if (readOne(a, &v)) { printRow(a, v); hits++; }
    delay(15);                       // >= 3.5 char times between frames
  }
  Serial.print(F("\n")); Serial.print(hits);
  Serial.print(F(" of ")); Serial.print(to - from + 1); Serial.println(F(" addresses returned a value\n"));
}

// The checks with answers you already know.  This is the fastest route to
// deciding which map applies.
void known() {
  uint16_t v;
  Serial.println(F("\n--- checks against values you already know ---\n"));

  Serial.print(F("reg 53   expect 40 if P-numbers are register addresses : "));
  if (readOne(53, &v)) {
    Serial.print(v);
    Serial.println(v == 40 ? F("   <== MATCH. try 59 (P59), 65 (P65), 109 (P109)") : F("   no match"));
  } else Serial.println(F("no response"));

  Serial.print(F("reg 59   P59 antifreeze, expect 30 (3.0C)             : "));
  if (readOne(59, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.print(F("reg 65   P65 min flow, expect 20 (L/min)              : "));
  if (readOne(65, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.print(F("reg 109  P109 range gate, expect 1                    : "));
  if (readOne(109, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.println();
  Serial.print(F("reg 257  cx50 says flow; compare against C13 on panel : "));
  if (readOne(257, &v)) { Serial.print(v); Serial.print(F("  = ")); Serial.print(v/10.0f,1); Serial.println(F(" L/min")); }
  else Serial.println(F("no response"));

  Serial.print(F("reg 213  cx34 says flow; compare against C13 on panel : "));
  if (readOne(213, &v)) { Serial.print(v); Serial.print(F("  = ")); Serial.print(v/10.0f,1); Serial.println(F(" L/min")); }
  else Serial.println(F("no response"));

  Serial.println();
  Serial.print(F("reg 202  vs C04 inlet on panel                        : "));
  if (readOne(202, &v)) { Serial.print((int16_t)v/10.0f,1); Serial.print(F("C / ")); Serial.print((int16_t)v/10.0f*9/5+32,1); Serial.println(F("F")); }
  else Serial.println(F("no response"));

  Serial.print(F("reg 203  vs C05 outlet on panel                       : "));
  if (readOne(203, &v)) { Serial.print((int16_t)v/10.0f,1); Serial.print(F("C / ")); Serial.print((int16_t)v/10.0f*9/5+32,1); Serial.println(F("F")); }
  else Serial.println(F("no response"));

  Serial.print(F("reg 209  cx50 says setpoint, expect 100 (10.0C)       : "));
  if (readOne(209, &v)) Serial.println((int16_t)v); else Serial.println(F("no response"));

  Serial.println(F("\nwhichever register reads close to a number on the panel is the"));
  Serial.println(F("one that means what the panel says. if a value looks off by one"));
  Serial.println(F("address, try +/-1 for the 0-based vs 1-based ambiguity.\n"));
}

// Repeatedly read a handful of addresses so you can watch which one tracks a
// value moving on the panel.  Any key stops it.
void watch(uint16_t *addrs, uint8_t n) {
  Serial.println(F("\nwatching; send any character to stop"));
  Serial.print(F("time\t"));
  for (uint8_t i = 0; i < n; i++) { Serial.print(addrs[i]); Serial.print(F("\t")); }
  Serial.println();
  while (!Serial.available()) {
    Serial.print(millis() / 1000); Serial.print(F("\t"));
    for (uint8_t i = 0; i < n; i++) {
      uint16_t v;
      if (readOne(addrs[i], &v)) Serial.print((int16_t)v); else Serial.print(F("--"));
      Serial.print(F("\t"));
      delay(15);
    }
    Serial.println();
    delay(2000);
  }
  while (Serial.available()) Serial.read();
  Serial.println(F("stopped\n"));
}

// Try every plausible DE pin at 9600 id 1.  A shield that directs itself
// answers on -1; one with a control pin answers on exactly that pin.
void sweepDe() {
  const int CAND[] = {-1, 2, 3, 4, 5, 6, 7, 8};
  Serial.println(F("\ntrying each DE pin at 9600 8N1, id 1..."));
  uint32_t savedBaud = curBaud; uint8_t savedId = curId;
  curBaud = 9600; curId = 1;
  for (uint8_t i = 0; i < sizeof(CAND) / sizeof(CAND[0]); i++) {
    applyDe(CAND[i]);
    applyLink();
    Serial.print(F("  DE=")); Serial.print(CAND[i]);
    Serial.print(CAND[i] < 0 ? F(" (auto) ... ") : F(" ... "));
    if (tryHere()) {
      Serial.println(F("RESPONDED"));
      Serial.print(F("\nDE pin is ")); Serial.println(deP);
      Serial.println(F("set `int deP` at the top of the sketch to this, then run 'k'\n"));
      return;
    }
    Serial.println(F("no"));
  }
  Serial.println(F("\nno DE pin worked at 9600. the baud may be wrong too --"));
  Serial.println(F("set a DE pin with 'd <n>' and run 'p' to sweep bauds.\n"));
  curBaud = savedBaud; curId = savedId; applyDe(2); applyLink();
}

// ---- console ----------------------------------------------------------------
void menu() {
  Serial.println(F("\n=== ChiltrixScan ==="));
  Serial.print(F("link: ")); Serial.print(curBaud);
  Serial.print(F(" 8N1  id=")); Serial.print(curId);
  Serial.print(F("  DE pin=")); Serial.println(deP);
  Serial.println(F("  d            find the DE pin            (start here)"));
  Serial.println(F("  d <n>        set DE pin, -1 for auto"));
  Serial.println(F("  p            probe id 1 at every baud   (then this)"));
  Serial.println(F("  P            probe id 1-32 at every baud (slow, minutes)"));
  Serial.println(F("  k            known-value checks         (do this second)"));
  Serial.println(F("  s            scan 0-400"));
  Serial.println(F("  s <a> <b>    scan a range"));
  Serial.println(F("  w            watch 202 203 257 213 256"));
  Serial.println(F("  w <a> ...    watch up to 6 addresses"));
  Serial.println(F("  b <baud>     set baud"));
  Serial.println(F("  i <id>       set slave id"));
  Serial.println(F("  ?            this menu"));
  Serial.println(F("read only: function code 3, never 6 or 16\n"));
}

void handle(char *line) {
  char cmd = line[0];
  char *arg = line + 1;
  while (*arg == ' ') arg++;

  if (cmd == 'd') { if (*arg) { applyDe(atoi(arg)); applyLink();
                                Serial.print(F("DE pin=")); Serial.println(deP); }
                    else sweepDe(); return; }
  if (cmd == 'p') { probe(1, 1); return; }
  if (cmd == 'P') { probe(1, 32); return; }
  if (cmd == 'k') { known(); return; }
  if (cmd == '?') { menu(); return; }
  if (cmd == 'b') { if (*arg) { curBaud = atol(arg); applyLink();
                                Serial.print(F("baud=")); Serial.println(curBaud); } return; }
  if (cmd == 'i') { if (*arg) { curId = atoi(arg); applyLink();
                                Serial.print(F("id=")); Serial.println(curId); } return; }
  if (cmd == 's') {
    uint16_t a = 0, b = 400;
    if (*arg) { a = atoi(strtok(arg, " ")); char *t = strtok(NULL, " "); b = t ? atoi(t) : a + 50; }
    scan(a, b); return;
  }
  if (cmd == 'w') {
    uint16_t addrs[6]; uint8_t n = 0;
    if (*arg) { char *t = strtok(arg, " "); while (t && n < 6) { addrs[n++] = atoi(t); t = strtok(NULL, " "); } }
    else { uint16_t d[] = {202, 203, 257, 213, 256}; for (n = 0; n < 5; n++) addrs[n] = d[n]; }
    watch(addrs, n); return;
  }
  Serial.println(F("? for the menu"));
}

char buf[64];
uint8_t blen = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000) { }
  int want = deP; deP = -1; applyDe(want);   // set up the pin through the one code path
  applyLink();
  menu();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (blen) { buf[blen] = 0; Serial.print(F("> ")); Serial.println(buf); handle(buf); blen = 0; }
    } else if (blen < sizeof(buf) - 1) {
      buf[blen++] = c;
    }
  }
}
