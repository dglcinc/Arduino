/*
 * CC1101_SpiDiag — raw-SPI diagnostic for the water-meter CC1101 bring-up.
 *
 * No RadioLib. Talks to the CC1101 directly over SPI to answer ONE question:
 * is the SPI link to the radio actually working? It performs the datasheet
 * manual-reset handshake (waiting for MISO to go low = chip ready), then reads
 * registers whose values are known:
 *   PARTNUM (0x30) -> 0x00 on a CC1101
 *   VERSION (0x31) -> 0x14 (some lots read 0x04 / 0x17) — anything but 00/FF is good
 *   IOCFG2  (0x00) -> 0x29  (power-on-reset default)
 *   FREQ2   (0x0D) -> 0x1E  (power-on-reset default)
 *
 * Interpreting the dump:
 *   all 0x00            -> MISO stuck LOW: bad CS, no chip response, dead/unpowered
 *                          chip, or MISO not reaching D12.
 *   all 0xFF            -> MISO stuck/floating HIGH: MISO disconnected (open
 *                          return path through the shifter).
 *   MISO-ready = NO     -> chip never pulled MISO low after reset: power/GND or
 *                          MISO path problem.
 *   correct defaults    -> SPI is fine; the problem (if any) is elsewhere.
 *
 * Wiring is identical to CC1101_WaterMeterTest (Appendix A.1):
 *   D13 SCK, D11 MOSI, D12 MISO, D10 CSN, 3V3->VCC, GND common both shifter sides.
 *
 * COMPILE / UPLOAD:
 *   arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi CC1101_SpiDiag
 *   arduino-cli upload  --fqbn arduino:renesas_uno:unor4wifi --port <port> CC1101_SpiDiag
 */

#include <SPI.h>

#define PIN_CS    10
#define PIN_MISO  12   // for the idle-level read and the reset-ready handshake

// 100 kHz — deliberately slow to tolerate the BSS138 shifter's soft edges.
SPISettings SPISET(100000, MSBFIRST, SPI_MODE0);

bool waitMisoLow(uint32_t timeoutMs) {
  uint32_t t = millis();
  while (digitalRead(PIN_MISO)) { if (millis() - t > timeoutMs) return false; }
  return true;
}

// Datasheet manual power-on reset. Returns true if the chip signalled ready
// (MISO went low) at both handshake points.
bool cc1101Reset(bool &ready1, bool &ready2) {
  digitalWrite(PIN_CS, HIGH); delayMicroseconds(5);
  digitalWrite(PIN_CS, LOW);  delayMicroseconds(10);
  digitalWrite(PIN_CS, HIGH); delayMicroseconds(45);

  digitalWrite(PIN_CS, LOW);
  ready1 = waitMisoLow(15);          // chip ready before the strobe
  SPI.beginTransaction(SPISET);
  SPI.transfer(0x30);                // SRES strobe
  SPI.endTransaction();
  ready2 = waitMisoLow(15);          // chip ready after reset completes
  digitalWrite(PIN_CS, HIGH);
  return ready1 && ready2;
}

// Status registers (0x30-0x3D) MUST be read with the burst bit (0xC0).
uint8_t readStatusReg(uint8_t addr) {
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(SPISET);
  SPI.transfer(addr | 0xC0);
  uint8_t v = SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
  return v;
}

// Config registers use a single-read (0x80).
uint8_t readConfigReg(uint8_t addr) {
  digitalWrite(PIN_CS, LOW);
  SPI.beginTransaction(SPISET);
  SPI.transfer(addr | 0x80);
  uint8_t v = SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(PIN_CS, HIGH);
  return v;
}

void hex2(uint8_t v) { if (v < 0x10) Serial.print('0'); Serial.print(v, HEX); }

// Passive MISO-leg probe, done ONCE before SPI claims D12. Distinguishes a
// short-to-GND from an open line from a healthy (externally pulled) line.
int g_misoPU    = -1;   // D12 read with the internal pull-up engaged
int g_misoFloat = -1;   // D12 read floating (no pull)

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}

  // Probe the MISO line as a plain GPIO first.
  pinMode(PIN_MISO, INPUT_PULLUP); delay(5); g_misoPU    = digitalRead(PIN_MISO);
  pinMode(PIN_MISO, INPUT);        delay(5); g_misoFloat = digitalRead(PIN_MISO);

  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  SPI.begin();   // reclaims D12 as CIPO

  Serial.println();
  Serial.println(F("=== CC1101 raw-SPI diagnostic ==="));
}

void loop() {
  bool r1, r2;
  cc1101Reset(r1, r2);

  uint8_t partnum = readStatusReg(0x30);
  uint8_t version = readStatusReg(0x31);
  uint8_t iocfg2  = readConfigReg(0x00);   // expect 0x29
  uint8_t freq2   = readConfigReg(0x0D);   // expect 0x1E

  Serial.print(F("MISO-leg pu="));      Serial.print(g_misoPU);
  Serial.print(F(" float="));           Serial.print(g_misoFloat);
  Serial.print(F(" | reset-ready: pre=")); Serial.print(r1 ? F("Y") : F("N"));
  Serial.print(F(" post="));            Serial.print(r2 ? F("Y") : F("N"));
  Serial.print(F(" | PARTNUM=0x"));     hex2(partnum);
  Serial.print(F(" VERSION=0x"));       hex2(version);
  Serial.print(F(" IOCFG2=0x"));        hex2(iocfg2);  Serial.print(F("(exp 29)"));
  Serial.print(F(" FREQ2=0x"));         hex2(freq2);   Serial.print(F("(exp 1E)"));

  bool ok = (partnum == 0x00) && (version != 0x00 && version != 0xFF)
            && (iocfg2 == 0x29) && (freq2 == 0x1E);
  Serial.println(ok ? F("  => SPI OK") : F("  => SPI FAIL"));

  delay(1000);
}
