// ArduinoPSI_impl.h — shared implementation for all ArduinoPSI sketches.
//
// This file is IDENTICAL in ArduinoPSI_BoilerLoop and ArduinoPSI_Domestic.
// Edit in one place and copy to the other. The only per-sketch differences
// are SENSOR_MAX_PSI and SENSOR_MAX_V, which the parent .ino defines before
// including this file.
//
// Hardware: Arduino UNO R4 WiFi + ratiometric analog pressure sensor on A0.
// See CLAUDE.md for full wiring and sensor details.

#pragma once

// ArduinoGraphics must be included BEFORE Arduino_LED_Matrix
#include "ArduinoGraphics.h"
#include "Arduino_LED_Matrix.h"
#include "WiFiS3.h"
#include "arduino_secrets.h"  // defines SECRET_SSID and SECRET_PASS

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

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void connectWiFi() {
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print("Connecting to SSID: ");
    Serial.println(ssid);
    WiFi.begin(ssid, pass);
    delay(10000);
  }
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

  connectWiFi();
  server.begin();
  printWifiStatus();
  matrix.begin();
}

void loop() {
  // Reconnect silently if WiFi dropped since last iteration.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — reconnecting...");
    connectWiFi();
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

  // Serve a pending HTTP request if one is waiting.
  WiFiClient client = server.available();
  if (client) {
    boolean currentLineIsBlank = true;
    while (client.connected()) {
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
          sprintf(jsonResponse, "{'psi' : %f}", psi);
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

  // Display current PSI for 1 s, then session-max PSI for 1 s.
  sprintf(displayText, " %d", (int)psi);
  matrix.beginDraw();
  matrix.stroke(0xFFFFFFFF);
  matrix.textFont(Font_4x6);
  matrix.beginText(1, 2, 0xFFFFFF);
  matrix.println(displayText);
  matrix.endText();
  matrix.endDraw();
  delay(1000);

  sprintf(displayText, "m%d", (int)psiMax);
  matrix.beginDraw();
  matrix.stroke(0xFFFFFFFF);
  matrix.textFont(Font_4x6);
  matrix.beginText(1, 2, 0xFFFFFF);
  matrix.println(displayText);
  matrix.endText();
  matrix.endDraw();
  delay(1000);
}
