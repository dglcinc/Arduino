// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// AirHandlerCal — bath calibration of every DS18B20 and every air NTC in one session, on the fabricated
// shield, rev A. Serial only, 115200, no WiFi.
//
// All the DS18B20s stay on the bus for the whole session. The board reads two NTCs at a time, so each
// NTC pair has its own 4-position plug (SUP, GND, RET, GND) and the plugs take turns in TB2 while every
// tip soaks in the bath. Comparing each NTC with the probes' mean from the same minutes removes any
// bath drift between swaps.
//
// At each bath:
//   1. Bundle all the tips with the Thermapen among them. Pair 1's plug stays in TB2.
//   2. Wait for "# pair 1: logging". That takes 6 min from the moment the mean stops moving: 1 min to
//      fill the flatness window, then 5 min flat. Note the Thermapen reading.
//   3. At "# pair N: 2 min logged, swap plug", pull the plug and fit the next. The sketch moves on to
//      the next pair by itself when a plug has been out for 1 s, and skips the first 15 s after it
//      goes in. After the last pair, pair 1 goes back in for the move to the next bath.
//
// Capture to a file on the Mac, with the board on USB:
//   arduino-cli monitor -p /dev/cu.usbmodemXXXX -c baudrate=115200 | tee ah-cal-90F.log
// Opening the port does not reset the R4, so type l for the ROM list and the column header.
//
// Each line is CSV; lines starting with # are notes:
//   t_s,pair,st,ret_cts,ret_ohm,ret_F,sup_cts,sup_ohm,sup_F,ds_mean,ds_spread,ds0,ds1,...
// st is out, settle, noprobe, soak, log or done; log and done lines are the measurement. Fit from
// ret_ohm and sup_ohm. The _F columns use the beta below for a look on the bench. dsN is column N of
// the ROM list, and a blank field is a channel or probe that did not read.
//
// Keys: 1-9 set the pair number, for a plug fitted out of turn or a restart mid-bath; l lists the ROMs
// and the header; r rescans the bus, for a probe plugged in late, and restarts the soak; s prints the
// DS2482's line status.
//
// Requires the OneWire library. OneWireBus.h is a symlink to ../AirHandler/OneWireBus.h.

#include "OneWireBus.h"
#include "CalTracker.h"

#define NTC_PAIRS      2          // plugs in rotation; change it to match the pairs in the bath
#define ONE_WIRE_PIN   2
#define NTC_R0_OHM     10000.0f   // NTC resistance at 25 C
#define NTC_BETA       3995.0f    // fitted 2026-10-20, same as AirHandler_MBR
#define NTC_RREF_OHM   10000.0f   // R1 and R2
#define ADC_MAX        16383.0f   // analogReadResolution(14)
#define ADC_OPEN       (ADC_MAX - 50)
#define ADC_SHORT      50

const int RET_PIN = A0;           // TB2 RET on the fabricated shield
const int SUP_PIN = A1;           // TB2 SUP

// 10 probes through the DS2482 take a few hundred ms to read and the conversion takes 750 ms, so a
// reading and the next conversion both fit in 2 s.
const unsigned long LINE_MS = 2000;

const int MAX_PROBES = 16;
typedef uint8_t DeviceAddress[8];

OneWireBus    bus(ONE_WIRE_PIN);
CalTracker    tracker(NTC_PAIRS);
DeviceAddress roms[MAX_PROBES];
int           nRoms = 0;

unsigned long retSum = 0, supSum = 0, adcCount = 0;
unsigned long lastLine = 0;
bool          retOpenSeen = false, supOpenSeen = false;
CalTracker::Status lastStatus = CalTracker::OUT;
int           lastPair = 0;
bool          doneNoted = false;

void printHex2(uint8_t v) {
  if (v < 16) Serial.print("0");
  Serial.print(v, HEX);
}

// One probe: its column, the printed form of the ROM, and the w1 name pivac's config uses, which
// reverses the six serial bytes.
void printProbe(int i) {
  Serial.print("# ds");
  Serial.print(i);
  Serial.print("  ");
  for (int b = 0; b < 8; b++) printHex2(roms[i][b]);
  Serial.print("  28-");
  for (int b = 6; b >= 1; b--) {
    if (roms[i][b] < 16) Serial.print("0");
    Serial.print(roms[i][b], HEX);
  }
  Serial.println();
}

void printHeader() {
  Serial.print("# 1-Wire master: ");
  Serial.println(bus.masterName());
  Serial.print("# ");
  Serial.print(nRoms);
  Serial.println(" DS18B20s");
  for (int i = 0; i < nRoms; i++) printProbe(i);
  Serial.print("t_s,pair,st,ret_cts,ret_ohm,ret_F,sup_cts,sup_ohm,sup_F,ds_mean,ds_spread");
  for (int i = 0; i < nRoms; i++) {
    Serial.print(",ds");
    Serial.print(i);
  }
  Serial.println();
}

void scanBus() {
  nRoms = 0;
  DeviceAddress a;
  bus.reset_search();
  while (nRoms < MAX_PROBES && bus.search(a)) {
    if (OneWire::crc8(a, 7) != a[7]) {
      Serial.println("# a ROM failed its CRC and was left out");
    } else if (a[0] != 0x28) {
      Serial.println("# a device that is not a DS18B20 was left out");
    } else {
      memcpy(roms[nRoms++], a, 8);
    }
  }
  for (int i = 0; i < nRoms; i++) bus.setResolution12(roms[i]);
  tracker.resetBath();
}

float ntcOhm(float counts) {
  return NTC_RREF_OHM * counts / (ADC_MAX - counts);
}

float ntcF(float ohm) {
  float k = 1.0f / (1.0f / 298.15f + logf(ohm / NTC_R0_OHM) / NTC_BETA);
  return (k - 273.15f) * 9.0f / 5.0f + 32.0f;
}

// Average counts, ohms and F; three blank fields when the channel was open or shorted at any point in
// this window, since a window that spans a plug's removal averages to a plausible wrong value.
void printChannel(unsigned long sum, bool openSeen) {
  float c = adcCount ? (float)sum / adcCount : 0;
  if (!adcCount || openSeen || c < ADC_SHORT) {
    Serial.print(",,,");
    return;
  }
  float ohm = ntcOhm(c);
  Serial.print(",");
  Serial.print(c, 2);
  Serial.print(",");
  Serial.print(ohm, 1);
  Serial.print(",");
  Serial.print(ntcF(ohm), 2);
}

void noteChange(CalTracker::Status st) {
  int p = tracker.currentPair();
  if (st == lastStatus && p == lastPair) return;
  if (p != lastPair) doneNoted = false;
  Serial.print("# pair ");
  Serial.print(p);
  switch (st) {
    case CalTracker::OUT:      Serial.println(": plug out or a channel open"); break;
    case CalTracker::SETTLE:   Serial.println(": plug in, settling 15 s"); break;
    case CalTracker::NO_PROBE: Serial.println(": a DS18B20 did not read; check the bus, then r"); break;
    case CalTracker::SOAK:     Serial.println(": waiting for the bath to hold flat"); break;
    case CalTracker::LOG:      Serial.println(": logging"); break;
    case CalTracker::DONE:
      if (!doneNoted) Serial.println(": 2 min logged, swap plug");
      else            Serial.println(": still logged");
      doneNoted = true;
      break;
  }
  lastStatus = st;
  lastPair = p;
}

void logLine() {
  float vals[MAX_PROBES];
  float sum = 0, lo = 0, hi = 0;
  int good = 0;
  for (int i = 0; i < nRoms; i++) {
    vals[i] = bus.readF(roms[i]);
    if (isnan(vals[i])) continue;
    if (!good || vals[i] < lo) lo = vals[i];
    if (!good || vals[i] > hi) hi = vals[i];
    sum += vals[i];
    good++;
  }
  bus.convertAll();

  bool allProbes = nRoms > 0 && good == nRoms;
  float mean = good ? sum / good : NAN;
  CalTracker::Status st = tracker.line(millis(), mean, allProbes);
  noteChange(st);

  Serial.print(millis() / 1000);
  Serial.print(",");
  Serial.print(tracker.currentPair());
  Serial.print(",");
  Serial.print(CalTracker::name(st));
  printChannel(retSum, retOpenSeen);
  printChannel(supSum, supOpenSeen);
  Serial.print(",");
  if (good) Serial.print(mean, 3);
  Serial.print(",");
  if (good) Serial.print(hi - lo, 2);
  for (int i = 0; i < nRoms; i++) {
    Serial.print(",");
    if (!isnan(vals[i])) Serial.print(vals[i], 3);
  }
  Serial.println();

  retSum = supSum = adcCount = 0;
  retOpenSeen = supOpenSeen = false;
}

void handleKey() {
  if (!Serial.available()) return;
  int c = Serial.read();
  if (c >= '1' && c <= '9') {
    tracker.setPair(c - '0');
    Serial.print("# pair set to ");
    Serial.println(tracker.currentPair());
  } else if (c == 'l') {
    printHeader();
  } else if (c == 'r') {
    bus.begin();
    scanBus();
    printHeader();
    bus.convertAll();
  } else if (c == 's') {
    uint8_t st = bus.resetStatus();
    if (st == 0xFF) {
      Serial.println("# bus status: no DS2482 answering");
    } else {
      Serial.print("# bus status 0x");
      Serial.print(st, HEX);
      Serial.print(": presence=");
      Serial.print(OneWireBus::stPresence(st));
      Serial.print(" short=");
      Serial.print(OneWireBus::stShort(st));
      Serial.print(" line=");
      Serial.println(OneWireBus::stLineHigh(st) ? "high" : "LOW");
    }
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  analogReadResolution(14);
  bus.begin();
  Serial.println();
  Serial.print("# AirHandlerCal: ");
  Serial.print(NTC_PAIRS);
  Serial.println(" NTC pairs on TB2, RET on A0, SUP on A1");
  scanBus();
  printHeader();
  bus.convertAll();
  lastLine = millis();
}

void loop() {
  int ret = analogRead(RET_PIN);
  int sup = analogRead(SUP_PIN);
  retSum += ret;
  supSum += sup;
  adcCount++;
  bool retOpen = ret > ADC_OPEN, supOpen = sup > ADC_OPEN;
  retOpenSeen = retOpenSeen || retOpen;
  supOpenSeen = supOpenSeen || supOpen;
  tracker.sample(millis(), retOpen, supOpen);

  if (millis() - lastLine >= LINE_MS) {
    lastLine += LINE_MS;
    logLine();
  }
  handleKey();
}
