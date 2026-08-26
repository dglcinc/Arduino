/*
 * ChiltrixModbus - Chiltrix CX75 Modbus RTU -> HTTP JSON bridge
 *
 * Arduino UNO R4 WiFi + DFRobot DFR0259 RS-485 shield.  Polls the chiller's
 * holding registers over Modbus RTU and serves the latest values as a single
 * single-quoted dict on GET / , which is the convention pivac.ArduinoSensor
 * parses (ast.literal_eval on a regex-extracted line, NOT json.loads).
 *
 * READ ONLY.  Function code 3 only, never 6 or 16.  Registers 140-146 are
 * writable on the chiller and include the cooling target and the on/off
 * switch; pivac is a monitoring system and this sketch does not write.
 *
 * ---- register map ----------------------------------------------------------
 * Addresses come from Chiltrix's own document, not a community map:
 *   "Cx34-1,2,3/Cx50-2/Cx35-1 Modbus Protocol and Connections Lists"
 *   (~/OneDrive - DGLC/Claude/HVAC Manuals/cx34-123&cx50-2&cx35-1 Modbus User
 *    Document.docx), which specifies 9600 8N1, slave ID 1, function 03.
 * It contradicts the jasipsw CX50-2 map on every address the two share, so the
 * addresses that map claimed - 203/204 water, 243/244 state and error, 257
 * flow, 258-260 speeds, 264 hours, 281 starts - are NOT polled here.  Chasing
 * them would publish plausible-but-wrong numbers, which is the failure mode
 * this system keeps getting bitten by.  ChiltrixScan still lists them as
 * candidates to hunt; nothing reaches Signal K until it is confirmed.
 *
 * ---- wiring ----------------------------------------------------------------
 * A3 and B3 on the chiller, and NO ground.  A/B only; leave the cable drain
 * grounded at the chiller end alone and off the Arduino.  Fail-safe bias stays
 * fitted at the Arduino end (680R +5V->A, 680R B->GND) because the DFR0259 has
 * no bias network and tri-states the driver between frames.
 *
 * Expect the idle A-B voltage to CHANGE when the wires move to A3/B3, and do
 * not read that as a fault.  On the old DA1/DA2 pair it sat at 0.44 V, which
 * solves to a ~120R terminator at the far end.  An unterminated pair loads the
 * bias divider only with the transceiver's ~12k, so several volts is the normal
 * reading there.  Anything comfortably above the 200 mV receiver threshold is
 * fine.  Do not add a terminator - it fights the bias resistors.
 */

#include <WiFiS3.h>
#include <ModbusMaster.h>
#include "arduino_secrets.h"      // #define SECRET_SSID "..." / #define SECRET_PASS "..."

// ---- link -------------------------------------------------------------------
// -1 when the shield's direction DIP switch is on AUTO, which is how this one
// is set. Use the pin number instead only if you switch it to MANU.
#define RS485_DE_PIN     -1
#define MODBUS_BAUD      9600     // fixed by the Chiltrix document
#define MODBUS_SLAVE     1        // ditto, for a single chiller
#define MODBUS_SERIAL    Serial1  // UNO R4: D0 = RX, D1 = TX, independent of USB Serial

#define POLL_MS          2000     // 7 transactions, roughly 0.4 s of bus time per poll
#define WIFI_RETRY_MS    30000
#define FRAME_GAP_MS     15       // >= 3.5 character times between frames at 9600

// ---- register addresses -----------------------------------------------------
#define REG_RW_BASE      140      // 140..146 contiguous, read in one transaction
#define REG_RW_COUNT     7
#define REG_AMBIENT      202      // int16, x0.1 C
#define REG_WATER_OUT    205      // int16, x0.1 C
#define REG_FLOW         213      // uint16, L/min
#define REG_COMP_HZ      227      // uint16, Hz, 0-80
#define REG_CURRENT      256      // uint16, A, 0-50
#define REG_WATER_IN     281      // int16, x0.1 C

// Two scales the document does not state.  It writes "(to get C divide by 10)"
// against every temperature and says nothing against these two, so raw units
// are what it implies.  Both have a free cross-check:
//   flow    -> C13 on the controller, read in the 1-2 minute pump-only window
//              at the start of a call.  It read >54 L/min after the Y-strainer
//              was cleaned, and P65 trips the P5 alarm at 20.
//   current -> electrical.emporia.house.chiltrix in Signal K, divided by 240 V.
//              Running power averages 1400-1600 W, so expect roughly 6 A.
// If either comes back 10x high, change its scale here to 0.1f.
#define FLOW_SCALE       1.0f
#define CURRENT_SCALE    1.0f

// One bit per Modbus transaction, so a partly failed poll is visible in the
// payload instead of hiding behind held values.
#define OKB_RW           0x01
#define OKB_AMBIENT      0x02
#define OKB_WATER_OUT    0x04
#define OKB_FLOW         0x08
#define OKB_COMP_HZ      0x10
#define OKB_CURRENT      0x20
#define OKB_WATER_IN     0x40
#define OKB_ALL          0x7F

ModbusMaster node;
WiFiServer server(80);

// ---- cached readings --------------------------------------------------------
// Each field holds its last good value.  A read that fails leaves the value
// alone and clears its ok_mask bit, so a consumer can tell fresh from held.
struct Reading {
  float    water_in_c  = 0;   // 281  return from the house, the warm side in cooling
  float    water_out_c = 0;   // 205  supply to the house, the cold side in cooling
  float    evap_dt_c   = 0;   // derived, in - out
  float    ambient_c   = 0;   // 202  the chiller's own outdoor air sensor
  float    flow_lpm    = 0;   // 213
  float    current_a   = 0;   // 256
  uint16_t comp_hz     = 0;   // 227  0 = compressor stopped
  uint16_t on_off      = 0;   // 140  0 = off, 1 = on
  uint16_t mode        = 0;   // 141  0 cool, 1 heat, 2 DHW, 3 cool+DHW, 4 heat+DHW
  uint16_t cool_set_c  = 0;   // 142  whole C - the setpoint rounding behind the E14 lockout
  uint16_t heat_set_c  = 0;   // 143  whole C
  uint16_t dhw_set_c   = 0;   // 144  whole C
  uint16_t heat_au     = 0;   // 145
  uint16_t dhw_au      = 0;   // 146
  uint8_t  ok_mask     = 0;   // OKB_* bits set on this poll
  uint8_t  ok          = 0;   // 1 only when every transaction succeeded
  uint8_t  last_err    = 0;   // ModbusMaster code of the first failure this poll
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
  node.clearResponseBuffer();
  uint8_t rc = node.readHoldingRegisters(addr, count);
  if (rc == node.ku8MBSuccess) {
    for (uint8_t i = 0; i < count; i++) out[i] = node.getResponseBuffer(i);
  }
  delay(FRAME_GAP_MS);
  return rc;
}

// Read one register into *out.  Sets its mask bit on success and records the
// first failure of the poll.
bool readReg(uint16_t addr, uint16_t *out, uint8_t bit, uint8_t *mask, uint8_t *firstErr) {
  uint16_t v;
  uint8_t rc = readBlock(addr, 1, &v);
  if (rc == node.ku8MBSuccess) { *out = v; *mask |= bit; return true; }
  if (!*firstErr) *firstErr = rc;
  return false;
}

void poll() {
  uint16_t b[REG_RW_COUNT];
  uint16_t v;
  uint8_t  mask = 0;
  uint8_t  firstErr = 0;
  uint8_t  rc;

  // 140..146  switch, mode, the three targets, the two AU flags.
  rc = readBlock(REG_RW_BASE, REG_RW_COUNT, b);
  if (rc == node.ku8MBSuccess) {
    r.on_off     = b[0];
    r.mode       = b[1];
    r.cool_set_c = b[2];
    r.heat_set_c = b[3];
    r.dhw_set_c  = b[4];
    r.heat_au    = b[5];
    r.dhw_au     = b[6];
    mask |= OKB_RW;
  } else if (!firstErr) firstErr = rc;

  // Temperatures are signed tenths of a degree C.
  if (readReg(REG_AMBIENT,   &v, OKB_AMBIENT,   &mask, &firstErr)) r.ambient_c   = (int16_t)v / 10.0f;
  if (readReg(REG_WATER_OUT, &v, OKB_WATER_OUT, &mask, &firstErr)) r.water_out_c = (int16_t)v / 10.0f;
  if (readReg(REG_WATER_IN,  &v, OKB_WATER_IN,  &mask, &firstErr)) r.water_in_c  = (int16_t)v / 10.0f;

  if (readReg(REG_FLOW,    &v, OKB_FLOW,    &mask, &firstErr)) r.flow_lpm  = v * FLOW_SCALE;
  if (readReg(REG_CURRENT, &v, OKB_CURRENT, &mask, &firstErr)) r.current_a = v * CURRENT_SCALE;
  if (readReg(REG_COMP_HZ, &v, OKB_COMP_HZ, &mask, &firstErr)) r.comp_hz   = v;

  // Warm side minus cold side, the same convention as the hydronic panels, so
  // it reads positive in cooling and inverts in heating.
  r.evap_dt_c = r.water_in_c - r.water_out_c;

  r.ok_mask  = mask;
  r.last_err = firstErr;
  r.ok       = (mask == OKB_ALL) ? 1 : 0;
}

// ---- HTTP -------------------------------------------------------------------
// One line, one dict, single-quoted keys.  pivac takes the first line matching
// /.*\{.*\}/ so nothing above this may contain a brace, and the whole dict has
// to fit on one line for the regex to close it.
//
// Temperatures are CELSIUS.  The pivac input must carry `scale: celsius`, or
// ArduinoSensor's default reads them as Fahrenheit.
void sendJson(WiFiClient &client) {
  char buf[640];
  snprintf(buf, sizeof(buf),
    "{'water_in_c': %.1f, 'water_out_c': %.1f, 'evap_dt_c': %.1f, 'ambient_c': %.1f, "
    "'flow_lpm': %.1f, 'current_a': %.1f, 'comp_hz': %u, "
    "'on_off': %u, 'mode': %u, 'cool_set_c': %u, 'heat_set_c': %u, 'dhw_set_c': %u, "
    "'heat_au': %u, 'dhw_au': %u, "
    "'ok': %u, 'ok_mask': %u, 'last_err': %u, 'uptime_ms': %lu}",
    r.water_in_c, r.water_out_c, r.evap_dt_c, r.ambient_c,
    r.flow_lpm, r.current_a, r.comp_hz,
    r.on_off, r.mode, r.cool_set_c, r.heat_set_c, r.dhw_set_c,
    r.heat_au, r.dhw_au,
    r.ok, r.ok_mask, r.last_err, millis());

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
