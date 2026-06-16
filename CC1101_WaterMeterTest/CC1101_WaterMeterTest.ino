/*
 * CC1101_WaterMeterTest — bring-up / detection sketch for the domestic water-meter
 * monitoring project (pivac docs/water-meter-monitoring-plan.md, Appendix A).
 *
 * GOAL: prove the CC1101 on this UNO R4 WiFi can HEAR the Sensus iPerl meter's
 * wireless M-Bus T1 telegrams at 868.95 MHz. It does NOT decode or decrypt —
 * that is the Pi's job later (wmbusmeters). Detection is deliberately
 * encoding-independent: it watches the channel RSSI and flags RF bursts that
 * rise above the noise floor. A POSITIVE result is bursts at a usable RSSI
 * (well above the noise floor) recurring on a regular ~4-8 s cadence — that is
 * the iPerl's beacon.
 *
 * WiFi: the board joins the house network and serves the live burst stats over
 * HTTP on port 80, so it can be carried to the meter on battery power and polled
 * remotely while the RSSI is watched. Single-quoted dict (house convention):
 *   {'floor':-112,'now':-107,'bursts':29,'since_last_s':2.3,'last_peak':-100,
 *    'last_gap_s':0.4,'last_dur_ms':2,'rate_4to8':5,'uptime_ms':123456}
 *   rate_4to8 = bursts in the last minute whose gap was a meter-like 3-9 s.
 *
 * WIRING (Appendix A.1 — CC1101 @3.3V <-> UNO R4 @5V via the 4-ch level shifter):
 *   D13 SCLK, D11 MOSI(SI), D12 MISO(SO), D10 CSN, 3V3->VCC, GND common both sides.
 *   Shifter LV=3V3, HV=5V, GND on BOTH sides. (GDO2 unused.) 868 MHz antenna.
 *
 * WiFi creds: gitignored arduino_secrets.h (SECRET_SSID / SECRET_PASS).
 * Library: RadioLib (global install). COMPILE/UPLOAD as the other sketches.
 */

#include <RadioLib.h>
#include "WiFiS3.h"
#include "arduino_secrets.h"   // SECRET_SSID / SECRET_PASS

// ---- Pin map (Appendix A.1) ----
#define PIN_CS    10   // CSN
#define PIN_GDO0   2   // GDO0 (RadioLib IRQ pin; unused for carrier-sense)
// Hardware SPI fixed on the UNO R4: SCLK=D13, MOSI=D11, MISO=D12.

CC1101 radio = new Module(PIN_CS, PIN_GDO0, RADIOLIB_NC, RADIOLIB_NC,
                          SPI, SPISettings(2000000, MSBFIRST, SPI_MODE0));

// ---- wM-Bus T1 channel ----
static const float WMBUS_FREQ_MHZ = 868.95f;
static const float WMBUS_BR_KBPS  = 103.0f;
static const float WMBUS_DEV_KHZ  = 50.0f;
static const float WMBUS_RXBW_KHZ = 325.0f;

// ---- Burst-detection tuning ----
static const float    BURST_MARGIN_DB = 10.0f;
static const uint32_t BURST_MIN_MS    = 2;
static const uint32_t BURST_MAX_MS    = 300;
static const uint32_t HEARTBEAT_MS    = 3000;
static const float    FLOOR_LEAK_DB   = 0.02f;

// ---- WiFi ----
char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;
WiFiServer server(80);
static const uint32_t WIFI_CONNECT_BUDGET_MS = 20000;
static const uint32_t HTTP_CLIENT_TIMEOUT_MS = 1500;

// ---- Runtime state ----
float    noiseFloor   = -110.0f;
bool     inBurst      = false;
float    burstPeak    = -150.0f;
uint32_t burstStartMs = 0;
uint32_t lastBurstMs  = 0;
uint32_t burstCount   = 0;
uint32_t lastHeartbeat = 0;
float    lastRssi      = -150.0f;
float    lastPeak      = -150.0f;   // peak of the most recent burst
uint32_t lastDurMs     = 0;
float    lastGapS      = 0.0f;

// Count meter-like bursts (gap 3-9 s) seen in the trailing 60 s.
static const uint8_t  RING = 32;
uint32_t meterLikeTimes[RING];
uint8_t  meterLikeHead = 0;

uint16_t meterLikeRate(uint32_t now) {
  uint16_t n = 0;
  for (uint8_t i = 0; i < RING; i++)
    if (meterLikeTimes[i] && now - meterLikeTimes[i] <= 60000) n++;
  return n;
}

float currentFreq = WMBUS_FREQ_MHZ;

// Retune the radio live (over HTTP) and reset all detection state so the floor
// and burst stats reflect the new band. CC1101 valid bands: 300-348, 387-464,
// 779-928 MHz (so 433.92 and 868.95 are both fine).
bool retune(float mhz) {
  if (radio.setFrequency(mhz) != RADIOLIB_ERR_NONE) return false;
  radio.receiveDirect();
  currentFreq = mhz;
  float minR = 0.0f;
  for (int i = 0; i < 100; i++) { float r = radio.getRSSI(); if (r < minR) minR = r; delay(2); }
  noiseFloor = minR;
  inBurst = false; burstCount = 0; lastBurstMs = 0;
  lastPeak = -150.0f; lastDurMs = 0; lastGapS = 0.0f;
  for (uint8_t i = 0; i < RING; i++) meterLikeTimes[i] = 0;
  return true;
}

void connectWiFi() {
  Serial.print(F("WiFi: joining ")); Serial.print(ssid); Serial.print(F(" ... "));
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_BUDGET_MS) {
    delay(300); Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F(" connected, IP ")); Serial.print(WiFi.localIP());
    Serial.print(F("  (AP RSSI ")); Serial.print(WiFi.RSSI()); Serial.println(F(" dBm)"));
    server.begin();
  } else {
    Serial.println(F(" FAILED (will keep listening; retries in background)."));
  }
}

void serveClient() {
  WiFiClient client = server.available();
  if (!client) return;
  uint32_t t0 = millis();
  // First line is the request, e.g. "GET /tune?mhz=433.92 HTTP/1.1".
  String reqLine = client.readStringUntil('\n');
  // Drain the rest of the headers.
  while (client.connected() && millis() - t0 < HTTP_CLIENT_TIMEOUT_MS) {
    String line = client.readStringUntil('\n');
    if (line.length() <= 1) break;
  }

  // Optional live retune: GET /tune?mhz=<freq>
  const char *note = "";
  int idx = reqLine.indexOf("mhz=");
  if (idx >= 0) {
    float mhz = reqLine.substring(idx + 4).toFloat();   // stops at the space
    note = (mhz > 0 && retune(mhz)) ? "tuned" : "tune-failed";
  }

  uint32_t now = millis();
  char body[240];
  snprintf(body, sizeof(body),
    "{'freq':%.2f,'floor':%d,'now':%d,'bursts':%lu,'since_last_s':%.1f,'last_peak':%d,"
    "'last_gap_s':%.1f,'last_dur_ms':%lu,'rate_4to8':%u,'note':'%s','uptime_ms':%lu}",
    (double)currentFreq, (int)noiseFloor, (int)lastRssi, (unsigned long)burstCount,
    lastBurstMs ? (now - lastBurstMs) / 1000.0 : -1.0,
    (int)lastPeak, (double)lastGapS, (unsigned long)lastDurMs,
    meterLikeRate(now), note, (unsigned long)now);
  client.println(F("HTTP/1.1 200 OK"));
  client.println(F("Content-Type: text/html"));
  client.println(F("Connection: close"));
  client.println();
  client.println(F("<!DOCTYPE HTML><html>"));
  client.println(body);
  client.println(F("</html>"));
  client.stop();
}

void halt(const __FlashStringHelper *msg) {
  pinMode(LED_BUILTIN, OUTPUT);
  while (true) {
    Serial.print(F("HALTED: ")); Serial.println(msg);
    digitalWrite(LED_BUILTIN, HIGH); delay(500);
    digitalWrite(LED_BUILTIN, LOW);  delay(500);
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}

  Serial.println();
  Serial.println(F("=== CC1101 water-meter detection test (WiFi) ==="));

  Serial.print(F("Initialising CC1101 ... "));
  int state = radio.begin();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.print(F("FAILED, code ")); Serial.println(state);
    halt(F("CC1101 not responding over SPI (check wiring/power/shifter)."));
  }
  Serial.println(F("OK."));

  if (radio.setFrequency(WMBUS_FREQ_MHZ)         != RADIOLIB_ERR_NONE) halt(F("setFrequency"));
  if (radio.setBitRate(WMBUS_BR_KBPS)            != RADIOLIB_ERR_NONE) halt(F("setBitRate"));
  if (radio.setFrequencyDeviation(WMBUS_DEV_KHZ) != RADIOLIB_ERR_NONE) halt(F("setFreqDev"));
  if (radio.setRxBandwidth(WMBUS_RXBW_KHZ)       != RADIOLIB_ERR_NONE) halt(F("setRxBw"));
  radio.setCrcFiltering(false);
  if (radio.receiveDirect() != RADIOLIB_ERR_NONE) halt(F("Could not enter RX."));

  Serial.print(F("Listening at ")); Serial.print(WMBUS_FREQ_MHZ); Serial.println(F(" MHz."));

  // Seed the noise floor from a short quiet sample.
  float minR = 0.0f;
  for (int i = 0; i < 100; i++) { float r = radio.getRSSI(); if (r < minR) minR = r; delay(2); }
  noiseFloor = minR;

  connectWiFi();
  lastHeartbeat = millis();
}

void loop() {
  // Keep WiFi alive (cheap, non-blocking unless actually dropped).
  static uint32_t lastWifiCheck = 0;
  uint32_t now = millis();
  if (now - lastWifiCheck > 5000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
  }
  serveClient();

  float rssi = radio.getRSSI();
  lastRssi = rssi;
  now = millis();

  if (rssi < noiseFloor) noiseFloor = rssi;
  else                   noiseFloor += FLOOR_LEAK_DB;

  if (rssi >= noiseFloor + BURST_MARGIN_DB) {
    if (!inBurst) { inBurst = true; burstStartMs = now; burstPeak = rssi; }
    else if (rssi > burstPeak) burstPeak = rssi;
  } else if (inBurst) {
    inBurst = false;
    uint32_t dur = now - burstStartMs;
    if (dur >= BURST_MIN_MS && dur <= BURST_MAX_MS) {
      burstCount++;
      lastPeak  = burstPeak;
      lastDurMs = dur;
      lastGapS  = lastBurstMs ? (now - lastBurstMs) / 1000.0f : 0.0f;
      if (lastBurstMs && lastGapS >= 3.0f && lastGapS <= 9.0f) {
        meterLikeTimes[meterLikeHead] = now;
        meterLikeHead = (meterLikeHead + 1) % RING;
      }
      Serial.print(F(">>> BURST #")); Serial.print(burstCount);
      Serial.print(F("  peak ")); Serial.print(burstPeak, 0); Serial.print(F(" dBm"));
      Serial.print(F("  dur "));  Serial.print(dur);          Serial.print(F(" ms"));
      Serial.print(F("  gap "));  Serial.print(lastGapS, 1);  Serial.print(F(" s"));
      Serial.print(F("  (floor ")); Serial.print(noiseFloor, 0); Serial.println(F(")"));
      lastBurstMs = now;
    }
  }

  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    Serial.print(F("listening... floor ")); Serial.print(noiseFloor, 0);
    Serial.print(F(" | now "));    Serial.print(rssi, 0);
    Serial.print(F(" | bursts ")); Serial.print(burstCount);
    Serial.print(F(" | meter-like/min ")); Serial.print(meterLikeRate(now));
    if (WiFi.status() == WL_CONNECTED) { Serial.print(F(" | IP ")); Serial.print(WiFi.localIP()); }
    Serial.println();
  }

  delay(2);
}
