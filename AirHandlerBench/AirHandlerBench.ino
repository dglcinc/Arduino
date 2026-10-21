// AirHandlerBench — bench test for the master bedroom air-handler node's PROTO-SHIELD.
//
// No WiFi and no HTTP: this sketch only proves the shield's wiring before the node's real sketch
// (pivac docs/mbr-air-handler-node-plan.md §4) is written. Open the Serial Monitor at 115200.
//
// At boot it searches the 1-Wire bus on D2 and lists every ROM with its CRC check, in the order
// the zone file wants them (28-byte form). Then once a second it prints one line:
//
//   ds[0] 71.83F  ds[1] 72.01F | A0 8143 cts 9940 ohm 77.3F | A1 8190 cts 9998 ohm 77.0F | D8 HIGH y2=0 edges=0
//
// Shield nets (pivac docs/mbr-air-handler-node-shield-review.md):
//   D2  1-Wire DQ, R7 4.7k pull-up to 5V, terminals 1 VDD, 2 DQ, 3 GND
//   A0  supply-air NTC, R8 10.0k to 5V, NTC to GND, C6 0.1uF, terminals 6 A0, 7 GND
//   A1  return-air NTC, R9 10.0k to 5V, NTC to GND, C7 0.1uF, terminals 11 A1, 10 GND
//   D8  Y2 relay contact to GND, R10 1k pull-up to 5V, terminals 5 D8, 4 GND
//
// Requires the OneWire and DallasTemperature libraries (the same pair ArduinoPSI_Domestic uses).

#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_PIN   2
#define Y2_PIN         8
#define NTC_R0_OHM     10000.0f   // NTC resistance at 25 C
#define NTC_BETA       3995.0f   // fitted 2026-10-20, same as AirHandler_MBR
#define NTC_RREF_OHM   10000.0f   // R8 and R9
#define ADC_MAX        16383.0f   // analogReadResolution(14)
#define ADC_SAMPLES    256

OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature ds(&oneWire);

DeviceAddress roms[8];
int           nRoms = 0;

int           y2Last = HIGH;
unsigned long y2Edges = 0;

void printRom(const DeviceAddress a) {
  Serial.print("{ ");
  for (int i = 0; i < 8; i++) {
    Serial.print("0x");
    if (a[i] < 16) Serial.print("0");
    Serial.print(a[i], HEX);
    if (i < 7) Serial.print(", ");
  }
  Serial.print(" }");
}

void scanBus() {
  nRoms = 0;
  DeviceAddress a;
  oneWire.reset_search();
  while (nRoms < 8 && oneWire.search(a)) {
    bool crcOk = OneWire::crc8(a, 7) == a[7];
    Serial.print("ROM ");
    Serial.print(nRoms);
    Serial.print("  ");
    printRom(a);
    Serial.print(crcOk ? "  CRC ok" : "  CRC BAD");
    Serial.println(a[0] == 0x28 ? "  DS18B20" : "  not a DS18B20");
    if (crcOk) memcpy(roms[nRoms++], a, 8);
  }
  if (nRoms == 0) Serial.println("No 1-Wire devices on D2.");
}

// NTC on the low side of the divider: 5V - Rref - pin - NTC - GND.
float ntcOhm(float counts) {
  return NTC_RREF_OHM * counts / (ADC_MAX - counts);
}

float ntcF(float counts) {
  if (counts < 50 || counts > ADC_MAX - 50) return NAN;
  float k = 1.0f / (1.0f / 298.15f + logf(ntcOhm(counts) / NTC_R0_OHM) / NTC_BETA);
  return (k - 273.15f) * 9.0f / 5.0f + 32.0f;
}

float readCounts(int pin) {
  unsigned long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) sum += analogRead(pin);
  return (float)sum / ADC_SAMPLES;
}

void printAnalog(const char *name, int pin) {
  float c = readCounts(pin);
  Serial.print(name);
  Serial.print(" ");
  Serial.print(c, 0);
  Serial.print(" cts ");
  if (c > ADC_MAX - 50) {
    Serial.print("OPEN (no NTC)");
  } else if (c < 50) {
    Serial.print("SHORT");
  } else {
    Serial.print(ntcOhm(c), 0);
    Serial.print(" ohm ");
    Serial.print(ntcF(c), 1);
    Serial.print("F");
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  pinMode(Y2_PIN, INPUT);               // R10 is the pull-up
  analogReadResolution(14);
  ds.begin();
  Serial.println();
  Serial.println("AirHandlerBench: 1-Wire on D2, NTCs on A0 and A1, Y2 on D8");
  scanBus();
  for (int i = 0; i < nRoms; i++) ds.setResolution(roms[i], 12);
  ds.setWaitForConversion(false);
  ds.requestTemperatures();
  y2Last = digitalRead(Y2_PIN);
}

void loop() {
  static unsigned long lastPrint = 0;

  // D8 every pass, so a bench jumper's bounce shows as extra edges.
  int y2 = digitalRead(Y2_PIN);
  if (y2 != y2Last) {
    y2Edges++;
    y2Last = y2;
  }

  if (millis() - lastPrint < 1000) return;
  lastPrint = millis();

  for (int i = 0; i < nRoms; i++) {
    float f = ds.getTempF(roms[i]);
    Serial.print("ds[");
    Serial.print(i);
    Serial.print("] ");
    if (f == DEVICE_DISCONNECTED_F) Serial.print("DISCONNECTED");
    else if (f == 185.0f) Serial.print("185.0F (power-on value)");
    else { Serial.print(f, 2); Serial.print("F"); }
    Serial.print("  ");
  }
  if (nRoms) Serial.print("| ");
  ds.requestTemperatures();             // 750 ms at 12 bits, ready by the next line

  printAnalog("A0", A0);
  Serial.print(" | ");
  printAnalog("A1", A1);
  Serial.print(" | D8 ");
  Serial.print(y2 == LOW ? "LOW  y2=1" : "HIGH y2=0");
  Serial.print(" edges=");
  Serial.println(y2Edges);

  // Type r in the Serial Monitor to rescan the bus after plugging in a probe.
  if (Serial.available()) {
    if (Serial.read() == 'r') {
      scanBus();
      for (int i = 0; i < nRoms; i++) ds.setResolution(roms[i], 12);
      ds.requestTemperatures();
    }
  }
}
