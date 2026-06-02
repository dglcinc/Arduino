// ArduinoPSI_impl.h — shared implementation for all ArduinoPSI sketches.
//
// ArduinoPSI_Domestic/ArduinoPSI_impl.h is a SYMLINK to this file, so both
// boards compile from this single source — edit here and both pick it up.
// Per-sketch differences are set by the parent .ino before the include:
//   - SENSOR_MAX_PSI / SENSOR_MAX_V (both boards)
//   - ONE_WIRE_BUS (Domestic only) compile-enables the DS18B20 recirc-loop
//     temperature; the BoilerLoop board leaves it undefined, so its
//     {'psi' : ...} response and code are completely unchanged.
//
// Hardware: Arduino UNO R4 WiFi + ratiometric analog pressure sensor on A0.
// See CLAUDE.md for full wiring and sensor details.

#pragma once

// ArduinoGraphics must be included BEFORE Arduino_LED_Matrix
#include "ArduinoGraphics.h"
#include "Arduino_LED_Matrix.h"
#include "WiFiS3.h"
#include "WDT.h"               // RA4M1 hardware watchdog
#include "arduino_secrets.h"  // defines SECRET_SSID and SECRET_PASS

// DS18B20 recirc-loop temperature — compiled in only when the parent sketch
// defines ONE_WIRE_BUS (the Domestic/DHW board). Requires the OneWire and
// DallasTemperature libraries: arduino-cli lib install "OneWire" "DallasTemperature".
#ifdef ONE_WIRE_BUS
#include <OneWire.h>
#include <DallasTemperature.h>
OneWire           oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
float         tempF        = DEVICE_DISCONNECTED_F;  // last good reading, Fahrenheit
unsigned long lastTempRead = 0;
#endif

// WiFi credentials — defined via arduino_secrets.h (not committed to git).
// See CLAUDE.md and arduino_secrets.h.example for the required format.
char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

char jsonResponse[100];
char output[100];
char displayText[100];

int status = WL_IDLE_STATUS;
WiFiServer server(80);
ArduinoLEDMatrix matrix;

// ---------------------------------------------------------------------------
// Sensor configuration
// SENSOR_MAX_PSI and SENSOR_MAX_V are defined by the parent .ino file.
//
// ADC note: the UNO R4 defaults to 10-bit analogRead() for UNO compatibility.
// We explicitly request 14-bit resolution in setup() with analogReadResolution(14).
// pinDigResolution must match — do not change one without the other.
// ---------------------------------------------------------------------------
const int   sensorPin        = A0;
const float sensorMinV       = 0.5f;
const float pinMaxV          = 5.0f;
const float pinDigResolution = 16384.0f;  // 2^14, matches analogReadResolution(14)
const float sensorDigTick    = pinDigResolution / pinMaxV;
const float digZeroPsiTicks  = sensorMinV * sensorDigTick;
const float sensorTotalTicks = pinDigResolution
                               - digZeroPsiTicks
                               - ((pinMaxV - SENSOR_MAX_V) * sensorDigTick);

int   sensorValue = 0;
float psi         = 0.0f;
float psiMax      = 0.0f;

// Display state — tracks which value is shown and when it last changed.
// Using millis() keeps the display alternating without blocking loop().
unsigned long lastDisplayChange = 0;
int           displayMode       = 0;  // 0 = current PSI, 1 = max PSI

// Reliability tuning -------------------------------------------------------
// RA4M1 hardware watchdog timeout (max ~5.6 s). loop() refreshes it every
// iteration, so the board self-resets only if a whole iteration wedges
// (e.g. a stuck HTTP client) for longer than this.
const uint32_t      WDT_TIMEOUT_MS           = 5000;
// How long to keep trying to (re)associate before giving up. On expiry the
// caller forces a full board reset rather than hammering a wedged module.
const unsigned long WIFI_CONNECT_BUDGET_MS   = 20000;
// Max time to service one HTTP client before dropping it. Bounds the
// previously-unbounded request loop so a stalled poller cannot hang the
// board. pivac polls complete in well under a second.
const unsigned long HTTP_CLIENT_TIMEOUT_MS   = 2000;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// (Re)connect to WiFi, bounded by budgetMs. Refreshes the watchdog while it
// waits so a slow-but-progressing association does not trip it. Returns true
// on success, false if the budget elapsed without connecting. A caller that
// gets false should force a full reset: a clean reboot clears a wedged WiFi
// module far more reliably than repeated WiFi.begin() calls.
bool connectWiFi(unsigned long budgetMs) {
  unsigned long start = millis();
  Serial.print("Connecting to SSID: ");
  Serial.println(ssid);
  WiFi.disconnect();             // clear any half-open / wedged association
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
  Serial.println(WiFi.SSID());
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);
  long rssi = WiFi.RSSI();
  Serial.print("Signal strength (RSSI): ");
  Serial.print(rssi);
  Serial.println(" dBm");
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // Arm the hardware watchdog FIRST so even a wedge in early init (missing
  // WiFi module, etc.) self-resets and retries instead of hanging forever.
  // loop() and connectWiFi() both refresh it well inside the timeout.
  if (!WDT.begin(WDT_TIMEOUT_MS)) {
    Serial.println("WDT.begin failed (timeout out of range?)");
  } else {
    Serial.println("Watchdog armed.");
  }

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    while (true);
  }

  String fv = WiFi.firmwareVersion();
  sprintf(output, "Firmware version: %s", fv.c_str());
  Serial.println(output);
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }

  // Use the R4's full 14-bit ADC instead of the 10-bit UNO-compatibility default.
  // pinDigResolution above (16384) must match this setting.
  analogReadResolution(14);

#ifdef ONE_WIRE_BUS
  // Non-blocking DS18B20: setWaitForConversion(false) makes requestTemperatures()
  // and the later getTemp call return immediately, so they never stall loop()/HTTP.
  ds18b20.begin();
  ds18b20.setResolution(12);            // 0.0625 C
  ds18b20.setWaitForConversion(false);
  ds18b20.requestTemperatures();        // kick off the first conversion
#endif

  // Block until the first association. connectWiFi() refreshes the watchdog
  // while it waits; if a budget elapses we just retry (e.g. AP still booting).
  while (!connectWiFi(WIFI_CONNECT_BUDGET_MS)) {
    Serial.println("Initial WiFi connect timed out; retrying...");
  }
  server.begin();
  printWifiStatus();
  matrix.begin();
}

void loop() {
  WDT.refresh();   // pet the watchdog once per iteration

  // Reconnect if WiFi dropped since last iteration.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — reconnecting...");
    if (!connectWiFi(WIFI_CONNECT_BUDGET_MS)) {
      Serial.println("Reconnect failed; resetting board.");
      delay(50);             // let serial flush before the reset
      NVIC_SystemReset();    // full board reset - does not return
    }
    printWifiStatus();
  }

  // Read and convert sensor value to PSI.
  sensorValue = analogRead(sensorPin);
  sprintf(output, "Zero = %f, RawValue = %d", digZeroPsiTicks, sensorValue);
  Serial.println(output);

  sensorValue -= (int)digZeroPsiTicks;
  if (sensorValue < 0) sensorValue = 0;

  psi = SENSOR_MAX_PSI * ((float)sensorValue / sensorTotalTicks);
  if (psi > psiMax) psiMax = psi;

  sprintf(output, "Sensor: %d  PSI: %f  TotalTicks: %f", sensorValue, psi, sensorTotalTicks);
  Serial.println(output);

#ifdef ONE_WIRE_BUS
  // Refresh the DS18B20 every ~3s without blocking: read the result of the
  // previous conversion, then immediately start the next one. Both calls return
  // immediately (setWaitForConversion(false)), so HTTP latency is unaffected.
  if (millis() - lastTempRead >= 3000) {
    tempF = ds18b20.getTempFByIndex(0);
    ds18b20.requestTemperatures();
    lastTempRead = millis();
  }
#endif

  // Serve a pending HTTP request if one is waiting.
  WiFiClient client = server.available();
  if (client) {
    unsigned long clientStart = millis();
    boolean currentLineIsBlank = true;
    while (client.connected()) {
      if (millis() - clientStart > HTTP_CLIENT_TIMEOUT_MS) break;  // drop a stalled client
      if (client.available()) {
        char c = client.read();
        if (c == '\n' && currentLineIsBlank) {
          // End of request headers — send response.
          // NOTE: single-quoted keys are intentional; pivac parses with
          // Python's ast.literal_eval, not a JSON parser. Do not change to
          // double quotes without updating ArduinoSensor.py in the pivac repo.
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: text/html");
          client.println("Connection: close");
          client.println();
          client.println("<!DOCTYPE HTML>");
          client.println("<html>");
          // 'uptime_ms' (millis() since boot) lets the pivac side tell a
          // self-reconnect (uptime keeps climbing) from a reboot/brownout
          // (uptime resets to ~0). Extra keys are harmless: pivac parses the
          // line with ast.literal_eval and ignores keys it doesn't use.
#ifdef ONE_WIRE_BUS
          sprintf(jsonResponse, "{'psi' : %f, 'temp' : %f, 'uptime_ms' : %lu}", psi, tempF, millis());
#else
          sprintf(jsonResponse, "{'psi' : %f, 'uptime_ms' : %lu}", psi, millis());
#endif
          client.println(jsonResponse);
          client.println("</html>");
          break;
        }
        if (c == '\n') {
          currentLineIsBlank = true;
        } else if (c != '\r') {
          currentLineIsBlank = false;
        }
      }
    }
    delay(1);
    client.stop();
  }

  // Alternate display between current PSI and session-max PSI every second.
  // millis()-based timing avoids blocking loop() so HTTP requests are served
  // promptly regardless of where the display cycle is.
  unsigned long now = millis();
  if (now - lastDisplayChange >= 1000) {
    displayMode = 1 - displayMode;
    lastDisplayChange = now;

    if (displayMode == 0) {
      sprintf(displayText, " %d", (int)psi);
    } else {
      sprintf(displayText, "m%d", (int)psiMax);
    }

    matrix.beginDraw();
    matrix.stroke(0xFFFFFFFF);
    matrix.textFont(Font_4x6);
    matrix.beginText(1, 2, 0xFFFFFF);
    matrix.println(displayText);
    matrix.endText();
    matrix.endDraw();
  }
}
