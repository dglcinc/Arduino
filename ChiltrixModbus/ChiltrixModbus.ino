/*
 * ChiltrixModbus - Chiltrix CX75 Modbus RTU -> HTTP JSON bridge
 *
 * Arduino UNO R4 WiFi + RS-485 shield.  Polls the chiller's holding registers
 * over Modbus RTU and serves the latest values as a single-quoted dict on
 * GET / , matching the convention pivac.ArduinoSensor already parses
 * (ast.literal_eval on a regex-extracted line - NOT json.loads).
 *
 * REGISTER MAP IS COMMUNITY-SOURCED AND UNVERIFIED ON THE CX75.
 * Validate every address against the controller's C-parameter display before
 * trusting it:  C04 <-> 202, C05 <-> 203, C13 <-> 257, C27 <-> 258.
 * If a value reads nonsense, try address +/- 1 (1-based vs 0-based documentation).
 */

#include <WiFiS3.h>
#include <ModbusMaster.h>
#include "arduino_secrets.h"      // #define SECRET_SSID "..."  / #define SECRET_PASS "..."

// ---- board wiring -----------------------------------------------------------
// -1 when the shield's direction DIP switch is on AUTO, which is how this one
// is set. Use the pin number instead only if you switch it to MANU.
#define RS485_DE_PIN     -1
#define MODBUS_BAUD      9600     // confirm at the controller; 19200 is the other common value
#define MODBUS_SLAVE     1
#define MODBUS_SERIAL    Serial1  // UNO R4: D0 = RX, D1 = TX, independent of USB Serial

#define POLL_MS          2000
#define WIFI_RETRY_MS    30000

ModbusMaster node;
WiFiServer server(80);

// ---- cached readings --------------------------------------------------------
struct Reading {
  float  water_in   = 0;   // C   reg 202 int16 x0.1
  float  water_out  = 0;   // C   reg 203 int16 x0.1
  float  ambient    = 0;   // C   reg 204 int16 x0.1
  float  flow_lpm   = 0;   // L/min reg 257 uint16 x0.1
  uint16_t power_w  = 0;   // W   reg 256
  uint16_t comp     = 0;   //     reg 258
  uint16_t fan      = 0;   //     reg 259
  uint16_t pump     = 0;   //     reg 260
  uint16_t state    = 0;   //     reg 243
  uint16_t errcode  = 0;   //     reg 244
  uint16_t run_hours = 0;  //     reg 264
  uint16_t starts   = 0;   //     reg 281
  uint8_t  ok       = 0;   // 1 = last poll succeeded end to end
  uint8_t  last_err = 0;   // ModbusMaster result code of the first failing block
};
Reading r;

unsigned long lastPoll = 0;
unsigned long lastWifiTry = 0;

// ---- RS-485 direction control ----------------------------------------------
void preTransmission()  { if (RS485_DE_PIN >= 0) digitalWrite(RS485_DE_PIN, HIGH); }
void postTransmission() { if (RS485_DE_PIN >= 0) digitalWrite(RS485_DE_PIN, LOW);  }

// Read `count` holding registers starting at `addr` into out[].  Returns the
// ModbusMaster result code (0 = success).
uint8_t readBlock(uint16_t addr, uint8_t count, uint16_t *out) {
  uint8_t rc = node.readHoldingRegisters(addr, count);
  if (rc == node.ku8MBSuccess) {
    for (uint8_t i = 0; i < count; i++) out[i] = node.getResponseBuffer(i);
  }
  return rc;
}

void poll() {
  uint16_t b[8];
  uint8_t rc;
  uint8_t firstErr = 0;

  // 202..204  inlet / outlet / ambient, int16 tenths of a degree C
  rc = readBlock(202, 3, b);
  if (rc == node.ku8MBSuccess) {
    r.water_in  = (int16_t)b[0] / 10.0f;
    r.water_out = (int16_t)b[1] / 10.0f;
    r.ambient   = (int16_t)b[2] / 10.0f;
  } else if (!firstErr) firstErr = rc;

  // 243..244  operating state / error code
  rc = readBlock(243, 2, b);
  if (rc == node.ku8MBSuccess) {
    r.state   = b[0];
    r.errcode = b[1];
  } else if (!firstErr) firstErr = rc;

  // 256..260  power / flow / compressor / fan / pump
  rc = readBlock(256, 5, b);
  if (rc == node.ku8MBSuccess) {
    r.power_w  = b[0];
    r.flow_lpm = b[1] / 10.0f;
    r.comp     = b[2];
    r.fan      = b[3];
    r.pump     = b[4];
  } else if (!firstErr) firstErr = rc;

  // 264 run hours, 281 compressor starts
  rc = readBlock(264, 1, b);
  if (rc == node.ku8MBSuccess) r.run_hours = b[0]; else if (!firstErr) firstErr = rc;
  rc = readBlock(281, 1, b);
  if (rc == node.ku8MBSuccess) r.starts = b[0];    else if (!firstErr) firstErr = rc;

  r.last_err = firstErr;
  r.ok = (firstErr == 0) ? 1 : 0;
}

// ---- HTTP -------------------------------------------------------------------
// One line, one dict, single-quoted keys.  pivac takes the first line matching
// /.*\{.*\}/ so nothing above this may contain braces.
void sendJson(WiFiClient &client) {
  char buf[420];
  snprintf(buf, sizeof(buf),
    "{'water_in': %.1f, 'water_out': %.1f, 'ambient': %.1f, "
    "'flow_lpm': %.1f, 'power_w': %u, 'comp': %u, 'fan': %u, 'pump': %u, "
    "'state': %u, 'errcode': %u, 'run_hours': %u, 'starts': %u, "
    "'ok': %u, 'last_err': %u, 'uptime_ms': %lu}",
    r.water_in, r.water_out, r.ambient,
    r.flow_lpm, r.power_w, r.comp, r.fan, r.pump,
    r.state, r.errcode, r.run_hours, r.starts,
    r.ok, r.last_err, millis());

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/plain");
  client.println("Connection: close");
  client.println();
  client.println(buf);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < WIFI_RETRY_MS && lastWifiTry != 0) return;
  lastWifiTry = millis();
  WiFi.begin(SECRET_SSID, SECRET_PASS);
}

void setup() {
  Serial.begin(115200);                 // USB, for debugging - separate from Serial1 on the R4

  if (RS485_DE_PIN >= 0) {
    pinMode(RS485_DE_PIN, OUTPUT);
    digitalWrite(RS485_DE_PIN, LOW);    // receive by default
  }

  MODBUS_SERIAL.begin(MODBUS_BAUD, SERIAL_8N1);
  node.begin(MODBUS_SLAVE, MODBUS_SERIAL);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  connectWifi();
  while (WiFi.status() != WL_CONNECTED) { delay(500); connectWifi(); }
  server.begin();
  Serial.print("ready, ip="); Serial.println(WiFi.localIP());
}

void loop() {
  connectWifi();

  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    poll();
  }

  WiFiClient client = server.available();
  if (client) {
    unsigned long t0 = millis();
    while (client.connected() && millis() - t0 < 1000) {
      if (client.available()) {
        // drain the request; we serve the same body for any path
        while (client.available()) client.read();
        sendJson(client);
        break;
      }
    }
    delay(1);
    client.stop();
  }
}
