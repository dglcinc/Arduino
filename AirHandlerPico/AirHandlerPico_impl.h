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
// The node only listens: nothing here writes to the SCB.
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

// Survive a reboot and clear at power-on, in the watchdog's scratch registers
const uint32_t SCRATCH_MAGIC = 0xA1B2C3D4;
#define SCRATCH_TAG    watchdog_hw->scratch[0]
#define SCRATCH_STALLS watchdog_hw->scratch[1]    // reboots since the last frame, for the back-off
#define SCRATCH_TOTAL  watchdog_hw->scratch[2]    // reboots since power-on, on the node's line

Adafruit_USBH_Host USBHost;
Adafruit_USBH_CDC  SerialHost;
WiFiServer         server(80);
WiFiUDP            udp;
ScbParser          scb;
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
  if (scb.status.y2 && !y2Active) y2StartMs = now;
  if (!scb.status.y2 && y2Active) y2TotalMs += now - y2StartMs;
  y2Active = scb.status.y2;
}

void serviceUsb(uint32_t now) {
  if (mountEvent) { mountEvent = false; Log.println("USB device mounted"); }
  if (umountEvent) { umountEvent = false; Log.println("USB device removed"); }
  if (!SerialHost.mounted()) return;
  uint8_t chunk[128];
  int avail = SerialHost.available();
  if (avail <= 0) return;
  int got = SerialHost.read(chunk, avail < (int)sizeof(chunk) ? avail : (int)sizeof(chunk));
  if (got > 0 && scb.feed(chunk, got) > 0) onFrame(now);
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
  const ScbStatus &s = scb.status;
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
    const ScbStatus &s = scb.status;
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

void serveClient() {
  WiFiClient client = server.accept();
  if (!client) return;
  uint32_t start = millis();
  bool blank = true;
  while (client.connected() && millis() - start < HTTP_TIMEOUT_MS) {
    serviceUsb(millis());                        // frames keep arriving while a slow client talks
    if (!client.available()) continue;
    char c = client.read();
    if (c == '\n' && blank) {
      char line[512];
      buildLine(line, sizeof(line), millis());
      client.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");
      client.println(line);
      break;
    }
    if (c == '\n') blank = true;
    else if (c != '\r') blank = false;
  }
  client.flush();
  client.stop();
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
  serviceAdc(now);
  serviceDs(now);
  showLeds(now);
  serveClient();
  watchSilence(now);

  if (now - lastStats >= STATS_MS) {
    lastStats = now;
    Log.printf("stats: %lu status, %lu log, %lu PC, %lu stray, %lu overflows, host %s, %.1fF %.1fF %.1fF %.1fF\n",
               (unsigned long)scb.nStatus, (unsigned long)scb.nLog, (unsigned long)scb.nPc,
               (unsigned long)scb.strayBytes, (unsigned long)scb.overflows,
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
