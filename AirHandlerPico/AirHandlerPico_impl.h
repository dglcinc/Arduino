// AirHandlerPico_impl.h: the air-handler node on the Pico W carrier, Rev B (pivac docs/air-handler-shield-pcb-plan.md §0).
//
// Each zone is a folder with its own .ino that defines the zone's constants and includes this file through
// a symlink. The zone .ino defines:
//   HOSTNAME        WiFi hostname shown in the UniFi client list
//   ROM_WSUP/WRET   DS18B20 ROMs for the coil's water supply and return. All zero reads the first two probes in
//                   search order, which is enough for the bench, and the log prints the ROMs to copy in.
//   NTC_R0_OHM, NTC_BETA, NTC_RREF_OHM
//
// Build: arduino-pico core, board "Raspberry Pi Pico W", USB Stack "Adafruit TinyUSB Host (native)":
//   arduino-cli compile -b rp2040:rp2040:rpipicow:usbstack=tinyusb_host AirHandlerPico_<ZONE>
// Needs the Adafruit TinyUSB library, and arduino_secrets.h in the zone's folder. To reflash, unplug 24 VAC and
// the USB-A cable first, because the micro-USB port and the converter share pin 40. Hold BOOTSEL and plug in.
//
// The node listens to the SCB until asked. Then ScbSetJob answers it just long enough to read or change its settings, in
// the sequence scripts/unico-scb.py proved, and stops. It cannot send the thermostat override or the test bytes.
//
//   GET /                          the JSON line below
//   GET /settings                  the stored settings the node last saw, and the last job's result
//   GET /settings/read?token=T     asks the SCB for them now; read /settings a few seconds later
//   GET /set?token=T&cfm.Y2=500    changes settings, in the Mac tool's names (ScbSettings.h); /settings shows the result
//
// T is SET_TOKEN in arduino_secrets.h, which needs at least 8 characters. Without it the node refuses every write. The
// token stops an accident on the LAN and travels in the clear, so it is no protection from anyone on the network.
//
// Pins: native USB is the host for the Unico's USB board, through TP2 and TP3. GP4 and GP5 are I2C to the DS2482
// and the ADS1115. GP8 Y1, GP9 Y2, GP10 W1, GP11 W2 and GP12 FAN drive the LEDs through 220 ohm. GP0 is a log.
//
// Response, one JSON line, which pivac.UnicoSCB reads as JSON and pivac.ArduinoSensor as a Python literal. A sensor
// that did not read is left out, and every SCB field is left out once no status frame has arrived for 10 s:
//   {"wsup":49.81,"wret":55.37,"aret":76.12,"asup":58.40,"g":1,"y1":1,"y2":0,"w1":0,"w2":0,"e":0,"cfm":300,"rpm":1045,
//    "cfm_g":300,"cfm_y1":300,"cfm_y2":600,"cfm_w1":300,"cfm_w2":600,"cfm_e":600,"flags":32,"scb_age":0.4,
//    "y2_s":4312,"rssi":-61,"uptime_ms":123456,"reboots":0}

#pragma once

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "Adafruit_TinyUSB.h"
#include "arduino_secrets.h"
#include "ScbParser.h"
#include "DS2482Bus.h"
#include "ADS1115.h"
#include "ScbSettings.h"
#include "ScbSetJob.h"

#ifndef LOG_HOST
#define LOG_HOST "10.0.0.84"
#endif
#define LOG_PORT 5515

const uint8_t PIN_SDA = 4, PIN_SCL = 5;
const uint8_t PIN_Y1 = 8, PIN_Y2 = 9, PIN_W1 = 10, PIN_W2 = 11, PIN_FAN = 12;

const uint32_t SCB_BAUD        = 4800;
const uint32_t WDT_MS          = 8000;
const uint32_t WIFI_JOIN_MS    = 20000;
const uint32_t WIFI_LOST_MS    = 60000;    // reboot: the boot path is the one that joins
const uint32_t SCB_REPORT_MS   = 10000;    // pivac's stale_s: past this the line drops the SCB fields
const uint32_t LED_STALE_MS    = 5000;     // the SCB's own 2 s gap every 14 s must not blink the LEDs
const uint32_t SILENCE_MS      = 20000;    // mounted and silent: the stall a reboot cleared on the bench
const uint32_t FIRST_FRAME_MS  = 30000;
const uint32_t MAX_BACKOFF_MS  = 600000;
const uint32_t ADC_PERIOD_MS   = 1000;
const uint32_t DS_PERIOD_MS    = 2000;
const uint32_t RESCAN_MS       = 30000;
const uint32_t HTTP_TIMEOUT_MS = 1000;
const uint32_t STATS_MS        = 300000;
const size_t   REQ_MAX         = 300;

// Survive a reboot and clear at power-on, in the watchdog's scratch registers
const uint32_t SCRATCH_MAGIC = 0xA1B2C3D4;
#define SCRATCH_TAG    watchdog_hw->scratch[0]
#define SCRATCH_STALLS watchdog_hw->scratch[1]    // reboots since the last frame, for the back-off
#define SCRATCH_TOTAL  watchdog_hw->scratch[2]    // reboots since power-on, on the node's line

Adafruit_USBH_Host USBHost;
Adafruit_USBH_CDC  SerialHost;
WiFiServer         server(80);
WiFiUDP            udp;
ScbParser          parser;
DS2482Bus          bus;
ADS1115            adc;

typedef uint8_t DeviceAddress[8];
const int     MAX_PROBES = 8;
DeviceAddress found[MAX_PROBES];
int           nFound = 0;
DeviceAddress romWsup = ROM_WSUP;
DeviceAddress romWret = ROM_WRET;
bool          romsByIndex = false;

float    wsup = NAN, wret = NAN, aret = NAN, asup = NAN;
uint32_t lastFrameMs = 0;
bool     haveFrame = false;
bool     y2Active = false;
uint32_t y2StartMs = 0, y2TotalMs = 0;
uint32_t lastAdc = 0, lastDs = 0, lastScan = 0, lastStats = 0, wifiLostAt = 0;
bool     wifiDown = false;

volatile bool mountEvent = false, umountEvent = false;

// A frame for the SCB, handed to core 1, which runs the USB stack and so does the write
uint8_t       txFrame[43];
volatile bool txPending = false;

void queueFrame(const uint8_t *f, size_t n) {
  if (txPending || n > sizeof(txFrame)) return;
  memcpy(txFrame, f, n);
  txPending = true;
}
ScbSetJob job(queueFrame);

// --- log: GP0 and UDP, one packet a line -----------------------------------------------------------

class UdpLog : public Print {
 public:
  size_t write(uint8_t c) override {
    Serial1.write(c);
    if (n < sizeof(line)) line[n++] = c;
    if (c == '\n' || n == sizeof(line)) flush();
    return 1;
  }
  void flush() override {
    if (n && WiFi.status() == WL_CONNECTED) {
      udp.beginPacket(LOG_HOST, LOG_PORT);
      udp.write(line, n);
      udp.endPacket();
    }
    n = 0;
  }
 private:
  uint8_t line[200];
  size_t  n = 0;
};
UdpLog Log;

// --- reboot, with a back-off so a dead SCB line cannot hold the sensors in a boot loop ------------

void rebootNow(const char *why, bool stall) {
  if (stall) SCRATCH_STALLS = SCRATCH_STALLS + 1;
  SCRATCH_TOTAL = SCRATCH_TOTAL + 1;
  Log.printf("REBOOT: %s\n", why);
  delay(200);
  rp2040.reboot();
}

uint32_t stallLimit(uint32_t base) {
  uint32_t n = SCRATCH_STALLS, limit = base;
  while (n-- && limit < MAX_BACKOFF_MS) limit *= 2;
  return limit < MAX_BACKOFF_MS ? limit : MAX_BACKOFF_MS;
}

// --- sensors --------------------------------------------------------------------------------------

float ntcF(float ratio) {                  // 3V3 - Rref - pin - NTC - GND
  if (ratio < 0.01f || ratio > 0.99f) return NAN;   // shorted or open sensor
  float ohm = NTC_RREF_OHM * ratio / (1.0f - ratio);
  float k = 1.0f / (1.0f / 298.15f + logf(ohm / NTC_R0_OHM) / NTC_BETA);
  return (k - 273.15f) * 9.0f / 5.0f + 32.0f;
}

bool romIsZero(const DeviceAddress a) {
  for (int i = 0; i < 8; i++) if (a[i]) return false;
  return true;
}

void scanBus() {
  DeviceAddress a;
  nFound = 0;
  bus.resetSearch();
  while (nFound < MAX_PROBES && bus.search(a)) {
    if (a[0] != 0x28) continue;
    memcpy(found[nFound++], a, 8);
    bus.setResolution12(a);
    Log.printf("probe %02X %02X %02X %02X %02X %02X %02X %02X\n", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  }
}

float probeF(const DeviceAddress rom, int index) {
  if (!romsByIndex) return bus.readF(rom);
  return index < nFound ? bus.readF(found[index]) : NAN;
}

void serviceDs(uint32_t now) {
  if (!bus.present()) {
    if (now - lastScan >= RESCAN_MS) {
      lastScan = now;
      if (bus.begin()) Log.println("DS2482 found");
    }
    return;
  }
  if (now - lastDs >= DS_PERIOD_MS) {
    lastDs = now;
    wsup = probeF(romWsup, 0);
    wret = probeF(romWret, 1);
    bus.convertAll();
  }
  if (romsByIndex && nFound < 2 && now - lastScan >= RESCAN_MS) {
    lastScan = now;
    scanBus();
  }
}

void serviceAdc(uint32_t now) {
  adc.tick();
  if (now - lastAdc >= ADC_PERIOD_MS) {
    lastAdc = now;
    float rs, rr;
    if (adc.period(rs, rr)) {
      asup = ntcF(rs);
      aret = ntcF(rr);
    } else {
      asup = aret = NAN;
    }
  }
}

// --- the SCB --------------------------------------------------------------------------------------

void onFrame(uint32_t now) {
  lastFrameMs = now;
  haveFrame = true;
  SCRATCH_STALLS = 0;
  if (parser.status.y2 && !y2Active) y2StartMs = now;
  if (!parser.status.y2 && y2Active) y2TotalMs += now - y2StartMs;
  y2Active = parser.status.y2;
}

void serviceUsb(uint32_t now) {
  if (mountEvent) { mountEvent = false; Log.println("USB device mounted"); }
  if (umountEvent) { umountEvent = false; Log.println("USB device removed"); }
  if (!SerialHost.mounted()) return;
  uint8_t chunk[128];
  int avail = SerialHost.available();
  if (avail <= 0) return;
  int got = SerialHost.read(chunk, avail < (int)sizeof(chunk) ? avail : (int)sizeof(chunk));
  if (got > 0 && parser.feed(chunk, got) > 0) {
    onFrame(now);
    job.onFrame(now, parser);
  }
}

// A mounted device that goes quiet is the stall the bench saw. With nothing plugged in the node keeps running.
void watchSilence(uint32_t now) {
  if (!SerialHost.mounted()) return;
  if (haveFrame) {
    if (now - lastFrameMs > stallLimit(SILENCE_MS)) rebootNow("no status frame, device still mounted", true);
  } else if (now > stallLimit(FIRST_FRAME_MS)) {
    rebootNow("no status frame since boot, device mounted", true);
  }
}

void showLeds(uint32_t now) {
  bool fresh = haveFrame && now - lastFrameMs < LED_STALE_MS;
  const ScbStatus &s = parser.status;
  digitalWrite(PIN_Y1, fresh && s.y1);
  digitalWrite(PIN_Y2, fresh && s.y2);
  digitalWrite(PIN_W1, fresh && s.w1);
  digitalWrite(PIN_W2, fresh && s.w2);
  digitalWrite(PIN_FAN, fresh ? (s.hasAirflow && s.actualRpm > 0) : (now / 1000) & 1);
}

// --- HTTP -----------------------------------------------------------------------------------------

size_t addF(char *b, size_t n, size_t len, const char *key, float v) {
  if (!isfinite(v) || n >= len) return n;
  return n + snprintf(b + n, len - n, "%s\"%s\":%.2f", n > 1 ? "," : "", key, v);
}
size_t addU(char *b, size_t n, size_t len, const char *key, unsigned long v) {
  if (n >= len) return n;
  return n + snprintf(b + n, len - n, "%s\"%s\":%lu", n > 1 ? "," : "", key, v);
}

void buildLine(char *b, size_t len, uint32_t now) {
  size_t n = snprintf(b, len, "{");
  n = addF(b, n, len, "wsup", wsup);
  n = addF(b, n, len, "wret", wret);
  n = addF(b, n, len, "aret", aret);
  n = addF(b, n, len, "asup", asup);
  if (haveFrame && now - lastFrameMs < SCB_REPORT_MS) {
    const ScbStatus &s = parser.status;
    n = addU(b, n, len, "g", s.g);
    n = addU(b, n, len, "y1", s.y1);
    n = addU(b, n, len, "y2", s.y2);
    n = addU(b, n, len, "w1", s.w1);
    n = addU(b, n, len, "w2", s.w2);
    n = addU(b, n, len, "e", s.e);
    if (s.hasAirflow) {
      static const char *STORED[6] = {"cfm_g", "cfm_y1", "cfm_y2", "cfm_w1", "cfm_w2", "cfm_e"};
      n = addU(b, n, len, "cfm", s.actualCfm);
      n = addU(b, n, len, "rpm", s.actualRpm);
      for (int k = 0; k < 6; k++) n = addU(b, n, len, STORED[k], s.stored[k]);
    }
    n = addU(b, n, len, "flags", s.flags);
    n = addF(b, n, len, "scb_age", (now - lastFrameMs) / 1000.0f);
  }
  n = addU(b, n, len, "y2_s", (y2TotalMs + (y2Active ? now - y2StartMs : 0)) / 1000);
  if (n < len) n += snprintf(b + n, len - n, ",\"rssi\":%ld", (long)WiFi.RSSI());
  n = addU(b, n, len, "uptime_ms", now);
  n = addU(b, n, len, "reboots", SCRATCH_TOTAL);
  if (n < len) snprintf(b + n, len - n, "}");
}

void settingsJson(char *b, size_t len, uint32_t now) {
  size_t n = snprintf(b, len, "{\"job\":\"%s\",\"result\":\"%s\"", job.busy() ? "working" : "idle", job.result);
  if (job.haveSettings && n < len) {
    const ScbSettings &s = job.settings;
    n += snprintf(b + n, len - n, ",\"age_s\":%lu,\"cfm\":{", (unsigned long)((now - job.settingsMs) / 1000));
    for (int k = 0; k < 6 && n < len; k++) n += snprintf(b + n, len - n, "%s\"%s\":%u", k ? "," : "", scb::MODE_KEYS[k], s.cfm[k]);
    if (n < len) n += snprintf(b + n, len - n, "},\"rpm\":{");
    for (int k = 0; k < 6 && n < len; k++) n += snprintf(b + n, len - n, "%s\"%s\":%u", k ? "," : "", scb::MODE_KEYS[k], s.rpm[k]);
    if (n < len) n += snprintf(b + n, len - n,
      "},\"air\":{\"percent\":%u,\"blower\":%u,\"sqft\":%u,\"occupants\":%u,\"runmin\":%u,\"interval\":%u,\"bedrooms\":%u,"
      "\"eac\":\"%s\",\"mode\":\"%s\"},\"auto\":{\"style\":\"%s\"},\"damper\":\"%s\"", s.percent, s.blower, s.sqft, s.occupants, s.runmin,
      s.interval, s.bedrooms, s.eac ? "on" : "off", s.autoMode ? "auto" : "fixed", s.energySaver ? "energysaver" : "maxcomfort",
      s.powerOpen ? "poweropen" : "powerclose");
  }
  if (n < len) snprintf(b + n, len - n, "}");
}

bool tokenOk(const char *t) {
#ifdef SET_TOKEN
  return strlen(SET_TOKEN) >= 8 && t && !strcmp(t, SET_TOKEN);
#else
  (void)t;
  return false;
#endif
}

// The path and query of "GET /path?a=b HTTP/1.1" are parsed in place. Returns the HTTP status and fills msg.
int route(char *target, char *body, size_t len, uint32_t now) {
  char *query = strchr(target, '?');
  if (query) *query++ = 0;
  if (!strcmp(target, "/")) { buildLine(body, len, now); return 200; }
  if (!strcmp(target, "/settings")) { settingsJson(body, len, now); return 200; }
  bool read = !strcmp(target, "/settings/read"), set = !strcmp(target, "/set");
  if (!read && !set) { snprintf(body, len, "{\"error\":\"not found\"}"); return 404; }

  const char *token = nullptr;
  const char *keys[ScbSetJob::MAX_CHANGES + 1], *vals[ScbSetJob::MAX_CHANGES + 1];
  size_t nPairs = 0;
  for (char *p = query, *next; p && *p; p = next) {
    next = strchr(p, '&');
    if (next) *next++ = 0;
    char *eq = strchr(p, '=');
    if (!eq) { snprintf(body, len, "{\"error\":\"malformed query\"}"); return 400; }
    *eq = 0;
    if (!strcmp(p, "token")) { token = eq + 1; continue; }
    if (nPairs > ScbSetJob::MAX_CHANGES) { snprintf(body, len, "{\"error\":\"too many settings\"}"); return 400; }
    keys[nPairs] = p;
    vals[nPairs++] = eq + 1;
  }
  if (!tokenOk(token)) { snprintf(body, len, "{\"error\":\"bad or missing token\"}"); return 403; }
  if (job.busy()) { snprintf(body, len, "{\"error\":\"a job is running\"}"); return 409; }
  job.clear();
  if (set) {
    if (!nPairs) { snprintf(body, len, "{\"error\":\"no settings given\"}"); return 400; }
    for (size_t k = 0; k < nPairs; k++) {
      if (!job.add(keys[k], vals[k])) { snprintf(body, len, "{\"error\":\"bad setting %s\"}", keys[k]); return 400; }
    }
  }
  if (!job.begin(now, SerialHost.mounted() && haveFrame && now - lastFrameMs < SCB_REPORT_MS)) {
    snprintf(body, len, "{\"error\":\"%s\"}", job.result);
    return 503;
  }
  Log.printf("job started: %s\n", set ? "set" : "read");
  snprintf(body, len, "{\"job\":\"started\"}");
  return 202;
}

void serveClient() {
  WiFiClient client = server.accept();
  if (!client) return;
  uint32_t start = millis();
  char req[REQ_MAX + 1];
  size_t nReq = 0;
  bool first = true, blank = true;
  while (client.connected() && millis() - start < HTTP_TIMEOUT_MS) {
    serviceUsb(millis());                        // frames keep arriving while a slow client talks
    if (!client.available()) continue;
    char c = client.read();
    if (first) {
      if (c == '\n') { first = false; req[nReq] = 0; }
      else if (nReq < REQ_MAX) req[nReq++] = c;
    }
    if (c == '\n' && blank) {
      static char body[768];
      int code = 400;
      char *target = strchr(req, ' ');
      if (!strncmp(req, "GET ", 4) && target) {
        target++;
        char *end = strchr(target, ' ');
        if (end) *end = 0;
        code = route(target, body, sizeof(body), millis());
      } else {
        snprintf(body, sizeof(body), "{\"error\":\"GET only\"}");
      }
      client.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n", code, code < 300 ? "OK" : "Error");
      client.println(body);
      break;
    }
    if (c == '\n') blank = true;
    else if (c != '\r') blank = false;
  }
  client.flush();
  client.stop();
}

void logJob() {
  static ScbSetJob::State shown = ScbSetJob::IDLE;
  if (job.state != shown) {
    shown = job.state;
    if (!job.busy()) Log.printf("job: %s\n", job.result);
  }
}

// --- core 0: everything but the USB stack ----------------------------------------------------------

void setup() {
  if (SCRATCH_TAG != SCRATCH_MAGIC) {          // power-on: the scratch registers hold nothing yet
    SCRATCH_TAG = SCRATCH_MAGIC;
    SCRATCH_STALLS = 0;
    SCRATCH_TOTAL = 0;
  }
  Serial1.begin(115200);
  for (uint8_t p : {PIN_Y1, PIN_Y2, PIN_W1, PIN_W2, PIN_FAN}) {
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
    gpio_set_drive_strength(p, GPIO_DRIVE_STRENGTH_8MA);
  }

  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.setClock(100000);
  Wire.setTimeout(25);
  Wire.begin();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_JOIN_MS) delay(100);
  server.begin();
  Log.printf("boot: %s %s, RSSI %d dBm, joined in %lu ms, reboots %lu, stalls %lu\n", HOSTNAME,
             WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned long)(millis() - t0),
             (unsigned long)SCRATCH_TOTAL, (unsigned long)SCRATCH_STALLS);

  bool ds = bus.begin();
  Log.println(ds ? "DS2482 found" : "DS2482 did not answer at 0x18, or failed its reset: check 3.3 V at U2 pins 1 and 3");
  romsByIndex = romIsZero(romWsup) || romIsZero(romWret);
  if (ds) {
    scanBus();
    bus.convertAll();
    if (!romsByIndex) {
      bus.setResolution12(romWsup);
      bus.setResolution12(romWret);
    }
  }
  lastDs = lastScan = lastAdc = lastStats = millis();
  rp2040.wdt_begin(WDT_MS);
}

void loop() {
  rp2040.wdt_reset();
  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    wifiDown = false;
  } else if (!wifiDown) {
    wifiDown = true;
    wifiLostAt = now;
  } else if (now - wifiLostAt > WIFI_LOST_MS) {
    rebootNow("WiFi lost", false);
  }

  serviceUsb(now);
  job.tick(now, parser);
  logJob();
  serviceAdc(now);
  serviceDs(now);
  showLeds(now);
  serveClient();
  watchSilence(now);

  if (now - lastStats >= STATS_MS) {
    lastStats = now;
    Log.printf("stats: %lu status, %lu log, %lu PC, %lu stray, %lu overflows, host %s, %.1fF %.1fF %.1fF %.1fF\n",
               (unsigned long)parser.nStatus, (unsigned long)parser.nLog, (unsigned long)parser.nPc,
               (unsigned long)parser.strayBytes, (unsigned long)parser.overflows,
               SerialHost.mounted() ? "mounted" : "absent", wsup, wret, asup, aret);
  }
}

// --- core 1: the USB host stack --------------------------------------------------------------------

void setup1() {
  USBHost.begin(0);                            // the RP2040's own USB controller
  SerialHost.begin(SCB_BAUD);                  // applied on mount; DTR and RTS rise on enumeration
}

void loop1() {
  USBHost.task();
  if (txPending) {                             // the frame goes out from the core that runs the USB stack
    if (SerialHost.mounted()) {
      SerialHost.write(txFrame, sizeof(txFrame));
      SerialHost.flush();
    }
    txPending = false;
  }
}

extern "C" {
void tuh_cdc_mount_cb(uint8_t idx) {
  SerialHost.mount(idx);
  mountEvent = true;
}
void tuh_cdc_umount_cb(uint8_t idx) {
  SerialHost.umount(idx);
  umountEvent = true;
}
}
