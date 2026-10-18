// AirHandler_impl.h — shared implementation for the air-handler nodes.
//
// Each zone is a folder with its own .ino that defines the zone's constants and then includes this
// file; AirHandler_<ZONE>/AirHandler_impl.h is a SYMLINK here, as the ArduinoPSI sketches do.
// Design: pivac docs/mbr-air-handler-node-plan.md §4. Shield: docs/mbr-air-handler-node-shield-review.md.
//
// The zone .ino defines:
//   ZONE_TAG        short name, also the WiFi hostname suffix ("MBR" -> airhandler-mbr)
//   HOSTNAME        WiFi hostname shown in the UniFi client list
//   ROM_WSUP/WRET   DS18B20 ROMs for the coil's water supply and return. All zero means "not yet
//                   known": the node then reads the first two probes on the bus in search order,
//                   which is enough for the bench, and the boot log prints both ROMs to copy in.
//   NTC_R0_OHM, NTC_BETA, NTC_RREF_OHM
//
// Pins (fixed by the shield): D2 1-Wire, A0 return-air NTC, A1 supply-air NTC, D8 Y2 contact to GND.
//
// Response, one line, single-quoted for pivac.ArduinoSensor's ast.literal_eval; a sensor that did not
// read correctly is left out of the line, never sent as a sentinel:
//   {'wsup' : 49.81, 'wret' : 55.37, 'aret' : 76.12, 'asup' : 58.40, 'y2' : 1, 'y2_s' : 4312, 'rssi' : -61, 'uptime_ms' : 84213021}

#pragma once

#include "ArduinoGraphics.h"     // before Arduino_LED_Matrix
#include "Arduino_LED_Matrix.h"
#include "WiFiS3.h"
#include "WDT.h"
#include <OneWire.h>
#include <DallasTemperature.h>
#include "arduino_secrets.h"     // SECRET_SSID, SECRET_PASS (gitignored)

const int ONE_WIRE_PIN = 2;
const int Y2_PIN       = 8;
const int ARET_PIN     = A0;
const int ASUP_PIN     = A1;

const float ADC_MAX = 16383.0f;            // analogReadResolution(14)

const uint32_t      WDT_TIMEOUT_MS         = 5000;
const unsigned long WIFI_CONNECT_BUDGET_MS = 20000;
const unsigned long HTTP_CLIENT_TIMEOUT_MS = 2000;
const unsigned long BRINGUP_MS             = 10000;
const unsigned long DS_PERIOD_MS           = 2000;
const unsigned long ADC_PERIOD_MS          = 1000;
const unsigned long WIFI_CHECK_MS          = 1000;
const unsigned long Y2_RELEASE_MS          = 100;

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

WiFiServer        server(80);
ArduinoLEDMatrix  matrix;
OneWire           oneWire(ONE_WIRE_PIN);
DallasTemperature ds(&oneWire);

DeviceAddress romWsup = ROM_WSUP;
DeviceAddress romWret = ROM_WRET;
bool          romsByIndex = false;          // true while the zone file still has zero ROMs

float wsup = NAN, wret = NAN, aret = NAN, asup = NAN;

// ADC accumulators, emptied once a second
unsigned long adcSumRet = 0, adcSumSup = 0, adcCount = 0;
unsigned long lastAdc = 0, lastDs = 0, lastWifiCheck = 0, lastMatrix = 0;
int           matrixStep = 0;

// Y2: a low sample asserts at once; release needs Y2_RELEASE_MS of continuous high samples
int           y2 = 0;
unsigned long y2HighSince = 0;
unsigned long y2CallStart = 0;
unsigned long y2TotalMs   = 0;

// ---------------------------------------------------------------------------

bool romIsZero(const DeviceAddress a) {
  for (int i = 0; i < 8; i++) if (a[i]) return false;
  return true;
}

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

float ntcOhm(float counts) {
  return NTC_RREF_OHM * counts / (ADC_MAX - counts);
}

// NTC on the low side of the divider: 5V - Rref - pin - NTC - GND.
float ntcF(float counts) {
  if (counts < 50 || counts > ADC_MAX - 50) return NAN;   // shorted or open sensor
  float k = 1.0f / (1.0f / 298.15f + logf(ntcOhm(counts) / NTC_R0_OHM) / NTC_BETA);
  return (k - 273.15f) * 9.0f / 5.0f + 32.0f;
}

float dsRead(const DeviceAddress rom, int index) {
  float f = romsByIndex ? ds.getTempFByIndex(index) : ds.getTempF(rom);
  if (f == DEVICE_DISCONNECTED_F || f == 185.0f) return NAN;
  return f;
}

// Before WiFi: list every ROM on the bus and the raw ADC counts for BRINGUP_MS, so the ROMs can be
// copied into the zone file (warm one probe in a hand and watch which reading rises).
void bringUp() {
  Serial.println();
  Serial.print("AirHandler ");
  Serial.print(ZONE_TAG);
  Serial.println(" bring-up: 1-Wire ROMs, then raw counts and temperatures");
  DeviceAddress a;
  int n = 0;
  oneWire.reset_search();
  while (oneWire.search(a)) {
    Serial.print("ROM ");
    Serial.print(n++);
    Serial.print("  ");
    printRom(a);
    Serial.println(OneWire::crc8(a, 7) == a[7] ? "  CRC ok" : "  CRC BAD");
  }
  if (n == 0) Serial.println("No 1-Wire devices on D2.");
  if (romsByIndex) Serial.println("Zone file ROMs are zero: wsup = probe 0, wret = probe 1 in search order.");

  unsigned long start = millis();
  while (millis() - start < BRINGUP_MS) {
    WDT.refresh();
    ds.requestTemperatures();
    delay(800);                                  // conversion, bring-up only
    WDT.refresh();
    Serial.print("ds0 ");
    Serial.print(ds.getTempFByIndex(0), 2);
    Serial.print("F  ds1 ");
    Serial.print(ds.getTempFByIndex(1), 2);
    Serial.print("F  A0 ");
    Serial.print(analogRead(ARET_PIN));
    Serial.print("  A1 ");
    Serial.print(analogRead(ASUP_PIN));
    Serial.print("  D8 ");
    Serial.println(digitalRead(Y2_PIN) == LOW ? "LOW" : "HIGH");
  }
}

bool connectWiFi(unsigned long budgetMs) {
  unsigned long start = millis();
  Serial.print("Connecting to SSID: ");
  Serial.println(ssid);
  WiFi.disconnect();
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > budgetMs) return false;
    WDT.refresh();
    delay(250);
  }
  return true;
}

void printWifiStatus() {
  Serial.print("SSID: ");
  Serial.print(WiFi.SSID());
  Serial.print("  IP: ");
  Serial.print(WiFi.localIP());
  Serial.print("  RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.print(" dBm  hostname: ");
  Serial.println(HOSTNAME);
}

void sampleY2() {
  unsigned long now = millis();
  if (digitalRead(Y2_PIN) == LOW) {
    y2HighSince = 0;
    if (!y2) {
      y2 = 1;
      y2CallStart = now;
    }
  } else if (y2) {
    if (!y2HighSince) y2HighSince = now;
    if (now - y2HighSince >= Y2_RELEASE_MS) {
      y2 = 0;
      y2TotalMs += y2HighSince - y2CallStart;   // the call ended at the first high sample
      y2HighSince = 0;
    }
  }
}

unsigned long y2Seconds() {
  unsigned long ms = y2TotalMs;
  if (y2) ms += millis() - y2CallStart;
  return ms / 1000;
}

void appendField(char *buf, size_t len, const char *key, float v) {
  if (!isfinite(v)) return;
  size_t n = strlen(buf);
  snprintf(buf + n, len - n, "%s'%s' : %.2f", n > 1 ? ", " : "", key, v);
}

void buildResponse(char *buf, size_t len) {
  snprintf(buf, len, "{");
  appendField(buf, len, "wsup", wsup);
  appendField(buf, len, "wret", wret);
  appendField(buf, len, "aret", aret);
  appendField(buf, len, "asup", asup);
  size_t n = strlen(buf);
  snprintf(buf + n, len - n, "%s'y2' : %d, 'y2_s' : %lu, 'rssi' : %ld, 'uptime_ms' : %lu}",
           n > 1 ? ", " : "", y2, y2Seconds(), (long)WiFi.RSSI(), millis());
}

void serveClient() {
  WiFiClient client = server.available();
  if (!client) return;
  unsigned long start = millis();
  bool blank = true;
  while (client.connected()) {
    if (millis() - start > HTTP_CLIENT_TIMEOUT_MS) break;
    sampleY2();                                   // keep the Y2 clock honest while serving
    if (!client.available()) continue;
    char c = client.read();
    if (c == '\n' && blank) {
      char line[256];
      buildResponse(line, sizeof(line));
      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: text/html");
      client.println("Connection: close");
      client.println();
      client.println("<!DOCTYPE HTML>");
      client.println("<html>");
      client.println(line);
      client.println("</html>");
      break;
    }
    if (c == '\n') blank = true;
    else if (c != '\r') blank = false;
  }
  delay(1);
  client.stop();
}

// The matrix cycles S (supply air), R (return air) and W (water ΔT, return minus supply), with a dot
// in the top-left corner while Y2 is asserted.
void drawMatrix() {
  char text[8];
  float v;
  char tag;
  switch (matrixStep) {
    case 0:  tag = 'S'; v = asup; break;
    case 1:  tag = 'R'; v = aret; break;
    default: tag = 'W'; v = wret - wsup; break;
  }
  matrixStep = (matrixStep + 1) % 3;
  if (isfinite(v)) snprintf(text, sizeof(text), "%c%d", tag, (int)lroundf(v));
  else             snprintf(text, sizeof(text), "%c--", tag);
  matrix.beginDraw();
  matrix.clear();
  matrix.stroke(0xFFFFFFFF);
  matrix.textFont(Font_4x6);
  matrix.beginText(1, 2, 0xFFFFFF);
  matrix.println(text);
  matrix.endText();
  if (y2) matrix.point(0, 0);
  matrix.endDraw();
}

// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) {}

  if (!WDT.begin(WDT_TIMEOUT_MS)) Serial.println("WDT.begin failed");
  else                            Serial.println("Watchdog armed.");

  pinMode(Y2_PIN, INPUT);                         // R10 is the pull-up
  analogReadResolution(14);
  ds.begin();
  romsByIndex = romIsZero(romWsup) || romIsZero(romWret);
  ds.setResolution(12);
  ds.setWaitForConversion(false);

  bringUp();

  ds.requestTemperatures();
  lastDs = millis();

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    while (true);                                 // the watchdog resets the board
  }
  while (!connectWiFi(WIFI_CONNECT_BUDGET_MS)) {
    Serial.println("Initial WiFi connect timed out; retrying...");
  }
  server.begin();
  printWifiStatus();
  matrix.begin();
  lastAdc = lastWifiCheck = lastMatrix = millis();
}

void loop() {
  WDT.refresh();
  unsigned long now = millis();

  if (now - lastWifiCheck >= WIFI_CHECK_MS) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      if (!connectWiFi(WIFI_CONNECT_BUDGET_MS)) {
        Serial.println("Reconnect failed; resetting board.");
        delay(50);
        NVIC_SystemReset();
      }
      printWifiStatus();
    }
  }

  sampleY2();

  adcSumRet += analogRead(ARET_PIN);
  adcSumSup += analogRead(ASUP_PIN);
  adcCount++;
  if (now - lastAdc >= ADC_PERIOD_MS) {
    float cRet = (float)adcSumRet / adcCount;
    float cSup = (float)adcSumSup / adcCount;
    aret = ntcF(cRet);
    asup = ntcF(cSup);
    Serial.print("A0 ");
    Serial.print(cRet, 0);
    Serial.print(" cts ");
    Serial.print(aret, 2);
    Serial.print("F  A1 ");
    Serial.print(cSup, 0);
    Serial.print(" cts ");
    Serial.print(asup, 2);
    Serial.print("F  (");
    Serial.print(adcCount);
    Serial.print(" samples)  y2 ");
    Serial.print(y2);
    Serial.print("  y2_s ");
    Serial.println(y2Seconds());
    adcSumRet = adcSumSup = adcCount = 0;
    lastAdc = now;
  }

  if (now - lastDs >= DS_PERIOD_MS) {
    wsup = dsRead(romWsup, 0);
    wret = dsRead(romWret, 1);
    ds.requestTemperatures();                     // 750 ms at 12 bits, done before the next read
    lastDs = now;
    Serial.print("wsup ");
    Serial.print(wsup, 2);
    Serial.print("F  wret ");
    Serial.print(wret, 2);
    Serial.println("F");
  }

  serveClient();

  if (now - lastMatrix >= 1000) {
    lastMatrix = now;
    drawMatrix();
  }
}
