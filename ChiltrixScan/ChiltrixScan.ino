/*
 * ChiltrixScan - interactive Modbus RTU register scanner for a Chiltrix CX
 *
 * Arduino UNO R4 (Minima or WiFi) + RS-485 shield, driven over USB from a
 * serial monitor at 115200.  No WiFi, no pivac.  Its only job is to find the
 * unit and identify which register is which.
 *
 * Why this exists: Chiltrix's own document now settles the addresses that
 * matter, but it lists only thirteen registers and omits the two this system
 * most wants - an operating state and a fault code.  So the job has changed
 * from "which community map applies" to "confirm the documented addresses,
 * then hunt the ones the document leaves out".
 *
 * The document is "Cx34-1,2,3/Cx50-2/Cx35-1 Modbus Protocol and Connections
 * Lists" (~/OneDrive - DGLC/Claude/HVAC Manuals/).  It specifies Modbus RTU
 * 9600/N/8/1, slave ID 1, function 03, which is where the probe starts.
 *
 * It agrees with gonzojive/heatpump (CX34) and contradicts
 * jasipsw/homeassistant-chiltrix-modbus (CX50-2) on every address the two
 * share, so treat any surviving jasipsw address below as a guess and nothing
 * more.  Neither map covers a CX75; only the unit settles it.
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

// ---- what each address is ---------------------------------------------------
// `doc` is Chiltrix's own document and is authoritative.  `guess` is the
// community maps, kept only for the addresses the document does not cover.
// Where the two columns disagree, the document wins.
struct Label { uint16_t addr; const char *doc; const char *guess; };
const Label LABELS[] = {
  {  53, "",                     "P53 EC pump min speed"     },
  {  59, "",                     "P59 antifreeze?"           },
  {  65, "",                     "P65 min water flow?"       },
  { 109, "",                     "P109 target range?"        },
  { 140, "SWITCH on/off R/W",    ""                          },
  { 141, "MODE 0c 1h 2d 3cd 4hd", ""                         },
  { 142, "AC COOL TARGET C R/W", ""                          },
  { 143, "AC HEAT TARGET C R/W", ""                          },
  { 144, "DHW TARGET C R/W",     ""                          },
  { 145, "AC heat AU mode R/W",  ""                          },
  { 146, "DHW AU mode R/W",      ""                          },
  { 202, "AMBIENT AIR T x0.1C",  "cx50 said water inlet"     },
  { 203, "",                     "cx50 outlet / cx34 suction" },
  { 204, "",                     "cx50 ambient / cx34 plate HX" },
  { 205, "OUTLET WATER T x0.1C", "cx50 said coil T"          },
  { 206, "",                     "cx50 discharge / cx34 solar" },
  { 207, "",                     "cx50 suction T"            },
  { 209, "",                     "cx50 setpoint / cx34 comp I" },
  { 213, "WATER FLOW L/min",     "cx50 said DHW setpoint"    },
  { 214, "",                     "cx50 antifreeze / cx34 P03" },
  { 225, "",                     "cx34 inner water flow sw"  },
  { 227, "COMPRESSOR FREQ Hz",   ""                          },
  { 243, "",                     "cx50 operating state"      },
  { 244, "",                     "cx50 error code"           },
  { 248, "",                     "cx34 internal pump speed"  },
  { 256, "INPUT AC CURRENT A",   "cx50 said power W"         },
  { 257, "",                     "cx50 flow / cx34 comp phase I" },
  { 258, "",                     "cx50 comp speed % / cx34 bus V" },
  { 259, "",                     "cx50 fan speed %"          },
  { 260, "",                     "cx50 pump speed % / cx34 IPM T" },
  { 261, "",                     "cx50 pressure / cx34 run hours" },
  { 264, "",                     "cx50 run hours (u32)"      },
  { 281, "INLET WATER T x0.1C",  "cx50 said comp starts"     },
  { 284, "",                     "cx34 current fault code"   },
  { 285, "",                     "cx50 defrost count"        },
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
  Serial.println(F("\nnothing answered. the pair is A3 and B3 on the chiller, A/B only,"));
  Serial.println(F("NO ground at the Arduino. check: A/B swapped, DE pin correct, and"));
  Serial.println(F("that the 680R fail-safe bias is still fitted at this end.\n"));
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
    if (l->doc[0])   { Serial.print(F("DOC:"));    Serial.print(l->doc); }
    if (l->guess[0]) { Serial.print(F("  guess:")); Serial.print(l->guess); }
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

// Print one temperature register in both scales.
void showTemp(const __FlashStringHelper *what, uint16_t addr) {
  uint16_t v;
  Serial.print(F("reg ")); Serial.print(addr); Serial.print(F("  ")); Serial.print(what);
  if (readOne(addr, &v)) {
    float c = (int16_t)v / 10.0f;
    Serial.print(c, 1); Serial.print(F(" C / ")); Serial.print(c * 9 / 5 + 32, 1); Serial.println(F(" F"));
  } else Serial.println(F("no response"));
}

// The documented registers, each against a value you can already see, plus the
// two scales the document leaves unstated.  Run this first once the unit
// answers; it is the fastest route to trusting the map.
void known() {
  uint16_t v, in = 0, out = 0;
  bool haveIn = false, haveOut = false;

  Serial.println(F("\n--- documented registers vs values you already know ---\n"));

  Serial.print(F("reg 140  switch, expect 1 while running              : "));
  if (readOne(140, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.print(F("reg 141  mode 0=cool 1=heat 2=DHW 3=c+D 4=h+D        : "));
  if (readOne(141, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.print(F("reg 142  AC COOL TARGET, expect 10 (whole C = 50F)   : "));
  if (readOne(142, &v)) {
    Serial.print(v);
    Serial.println(v == 10 ? F("   <== MATCHES the panel target") : F("   check the panel"));
  } else Serial.println(F("no response"));

  Serial.println();
  showTemp(F("INLET water  vs C04 on panel        : "), 281);
  showTemp(F("OUTLET water vs C05 on panel        : "), 205);
  showTemp(F("AMBIENT air  vs the outdoor reading : "), 202);

  // Inlet is the return from the house and runs warmer than outlet in cooling.
  // A negative delta here means the two addresses are swapped, or the unit is
  // in heating.  Expect roughly 2-4 C at part load, 4.1 C at full output.
  if (readOne(281, &in))  haveIn  = true;
  if (readOne(205, &out)) haveOut = true;
  if (haveIn && haveOut) {
    float dt = ((int16_t)in - (int16_t)out) / 10.0f;
    Serial.print(F("\nevaporator delta-T (281 - 205)                       : "));
    Serial.print(dt, 1); Serial.print(F(" C / ")); Serial.print(dt * 9 / 5, 1); Serial.println(F(" F"));
    Serial.println(F("  positive in cooling. negative = swapped addresses, or heating."));
  }

  Serial.println(F("\n--- the two scales the document does not state ---\n"));

  Serial.print(F("reg 213  WATER FLOW vs C13 on the panel              : "));
  if (readOne(213, &v)) {
    Serial.print(F("raw ")); Serial.print(v);
    Serial.print(F("  -> ")); Serial.print(v); Serial.print(F(" or "));
    Serial.print(v / 10.0f, 1); Serial.println(F(" L/min"));
    Serial.println(F("  C13 read >54 after the strainer was cleaned; P65 trips at 20."));
    Serial.println(F("  C13 reads 0 at idle, so take this in the pump-only window."));
  } else Serial.println(F("no response"));

  Serial.print(F("\nreg 256  INPUT AC CURRENT vs Emporia / 240 V         : "));
  if (readOne(256, &v)) {
    Serial.print(F("raw ")); Serial.print(v);
    Serial.print(F("  -> ")); Serial.print(v); Serial.print(F(" or "));
    Serial.print(v / 10.0f, 1); Serial.println(F(" A"));
    Serial.println(F("  running power is 1400-1600 W, so expect about 6 A."));
  } else Serial.println(F("no response"));

  Serial.print(F("\nreg 227  COMPRESSOR FREQ, 0 idle, up to 80 Hz        : "));
  if (readOne(227, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.println(F("\n--- what the document omits, and this system wants ---\n"));

  Serial.print(F("reg 53   expect 40 if P-numbers are register addrs   : "));
  if (readOne(53, &v)) {
    Serial.print(v);
    Serial.println(v == 40 ? F("   <== MATCH. P59, P65, P109 are then readable")
                           : F("   no match; P-parameters are elsewhere"));
  } else Serial.println(F("no response"));

  Serial.print(F("reg 59   P59 antifreeze, expect 30 (3.0C)            : "));
  if (readOne(59, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.print(F("reg 65   P65 min water flow, expect 20               : "));
  if (readOne(65, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.print(F("reg 109  P109 range gate, expect 1                   : "));
  if (readOne(109, &v)) Serial.println(v); else Serial.println(F("no response"));

  Serial.println(F("\nfault and state are NOT in the document. candidates:"));
  Serial.print(F("reg 243  cx50 called this operating state           : "));
  if (readOne(243, &v)) Serial.println(v); else Serial.println(F("no response"));
  Serial.print(F("reg 244  cx50 called this error code                : "));
  if (readOne(244, &v)) Serial.println(v); else Serial.println(F("no response"));
  Serial.print(F("reg 284  cx34 called this current fault code        : "));
  if (readOne(284, &v)) Serial.println(v); else Serial.println(F("no response"));
  Serial.println(F("  the way to settle these is to read them while the panel shows"));
  Serial.println(F("  a known alarm - P5 low flow or E14 antifreeze - not while idle.\n"));

  Serial.println(F("a register that reads close to a number on the panel means what the"));
  Serial.println(F("panel says. if a value looks off by one address, try +/-1 for the"));
  Serial.println(F("0-based vs 1-based ambiguity.\n"));
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
  Serial.println(F("  k            documented-register checks (do this second)"));
  Serial.println(F("  s            scan 0-400"));
  Serial.println(F("  s <a> <b>    scan a range"));
  Serial.println(F("  w            watch 281 205 213 227 256 (in/out/flow/Hz/A)"));
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
    else { uint16_t d[] = {281, 205, 213, 227, 256}; for (n = 0; n < 5; n++) addrs[n] = d[n]; }
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
