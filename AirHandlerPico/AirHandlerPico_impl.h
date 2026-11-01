// AirHandlerPico_impl.h: the AirMaster firmware, on the Pico W carrier, Rev B (pivac docs/air-handler-shield-pcb-plan.md §0).
//
// The product sketch is AirMaster/. Each zone or bench folder is a .ino that defines its constants and includes this file
// through a symlink. All of these are optional:
//   HOSTNAME        the unit's default name, before anyone names it
//   ROM_WSUP/WRET   DS18B20 ROMs for the coil's water supply and return. All zero reads the first two probes in search
//                   order until the unit's Sensors page says otherwise.
//   NTC_R0_OHM, NTC_BETA, NTC_RREF_OHM
//   LOG_HOST        send the log to this host by UDP as well as GP0. Leave it out of anything that leaves the house.
//
// Build: arduino-pico core, board "Raspberry Pi Pico W", USB Stack "Adafruit TinyUSB Host (native)", 1 MB filesystem:
//   arduino-cli compile -b rp2040:rp2040:rpipicow:usbstack=tinyusb_host,flash=2097152_1048576 AirMaster
// Needs the Adafruit TinyUSB library. To reflash over USB, unplug 24 VAC and the USB-A cable first, because the micro-USB
// port and the converter share pin 40. Hold BOOTSEL and plug in. AirMaster/factory does this for a new unit.
//
// Connecting. The factory writes each unit an identity holding its setup password (UnitCore.h). The unit starts its own
// WiFi network, AirMaster-<unit id>, when:
//   - it has no saved home WiFi: first setup. It stays up.
//   - it cannot join its saved WiFi. It reboots to try again after 10 minutes with nobody connected.
//   - it has been powered up three times within 30 s of each other: recovery, for a forgotten password. Same 10 minutes.
// It tries a new home WiFi by rebooting onto it, since the radio holds one channel and a phone on the setup network would
// drop anyway; a failure brings the setup network back with the reason. The readings carry on through all of this.
//
// The web page is compiled in, compressed (web_page.h, from AirMaster/web). Viewing is open to the network. Changes need
// the admin password, the login cookie and the page's X-AirMaster header. The page goes over plain HTTP.
//
// The unit listens to the SCB until asked. Then ScbSetJob answers it just long enough to read or change its settings, in
// the sequence scripts/unico-scb.py proved, and stops. It cannot send the thermostat override or the test bytes.
//
//   GET /                   the page for a browser; the JSON line below for anything else, which is what pivac reads
//   GET /data               the JSON line
//   GET /settings           the stored settings the unit last saw, and the last job's result
//   GET /api/info           name, mode, sensors, WiFi, and the networks seen when on the setup network
//   POST /api/login, /api/logout, /api/setup, /api/unit, /api/settings/read, /api/set, /api/restore, /api/update,
//        /api/reboot, /api/factory-reset
//
// Pins: native USB is the host for the Unico's USB board, through TP2 and TP3. GP4 and GP5 are I2C to the DS2482
// and the ADS1115. GP8 Y1, GP9 Y2, GP10 W1, GP11 W2 and GP12 FAN drive the LEDs through 220 ohm. GP0 is a log.
//
// The JSON line, which pivac.UnicoSCB reads as JSON and pivac.ArduinoSensor as a Python literal. A sensor that did not read
// is left out, and every SCB field is left out once no status frame has arrived for 10 s. Temperatures include the
// offsets saved on the unit, and cal is 1 when any are set, so pivac's own offsets stay zero for such a unit:
//   {"wsup":49.81,"wret":55.37,"aret":76.12,"asup":58.40,"g":1,"y1":1,"y2":0,"w1":0,"w2":0,"e":0,"cfm":300,"rpm":1045,
//    "cfm_g":300,"cfm_y1":300,"cfm_y2":600,"cfm_w1":300,"cfm_w2":600,"cfm_e":600,"flags":32,"scb_age":0.4,
//    "y2_s":4312,"cal":1,"rssi":-61,"uptime_ms":123456,"reboots":0}

#pragma once

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include <stdarg.h>
#include <LittleFS.h>
#include <LEAmDNS.h>
#include <DNSServer.h>
#include <Updater.h>
#include "Adafruit_TinyUSB.h"
#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"              // a bench or zone build's WiFi, used until the unit has saved its own
#endif
#include "ScbParser.h"
#include "DS2482Bus.h"
#include "ADS1115.h"
#include "ScbSettings.h"
#include "ScbSetJob.h"
#include "UnitCore.h"
#include "HttpRequest.h"
#include "web_page.h"

#define FW_VERSION "1.0.0"
#define LOG_PORT 5515
#ifndef ROM_WSUP
#define ROM_WSUP { 0, 0, 0, 0, 0, 0, 0, 0 }
#endif
#ifndef ROM_WRET
#define ROM_WRET { 0, 0, 0, 0, 0, 0, 0, 0 }
#endif

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
const uint32_t HTTP_TIMEOUT_MS = 2000;
const uint32_t AP_IDLE_MS      = 600000;   // setup network with nobody on it, when there is a saved WiFi to retry
const uint32_t BOOT_WINDOW_MS  = 30000;    // a power-up lasting less counts toward recovery
const int      RECOVERY_BOOTS  = 3;
const uint32_t SESSION_MS      = 43200000; // 12 h
const uint32_t LOGIN_DELAY_MS  = 2000;     // after a wrong password
const size_t   FORM_MAX        = 1500;
const size_t   FIRMWARE_MAX    = 1044480;  // the sketch space with the 1 MB filesystem
const uint32_t STATS_MS        = 300000;

// Survive a reboot and clear at power-on, in the watchdog's scratch registers
const uint32_t SCRATCH_MAGIC = 0xA1B2C3D4;
#define SCRATCH_TAG    watchdog_hw->scratch[0]
#define SCRATCH_STALLS watchdog_hw->scratch[1]    // reboots since the last frame, for the back-off
#define SCRATCH_TOTAL  watchdog_hw->scratch[2]    // reboots since power-on, on the node's line
#define SCRATCH_POWER_ON watchdog_hw->scratch[3]  // this boot was a power-up, which counts toward recovery

Adafruit_USBH_Host USBHost;
Adafruit_USBH_CDC  SerialHost;
WiFiServer         server(80);
DNSServer          dns;
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

float    foundF[MAX_PROBES];
float    wsup = NAN, wret = NAN, aret = NAN, asup = NAN;
uint32_t lastFrameMs = 0;
bool     haveFrame = false;
bool     y2Active = false;
uint32_t y2StartMs = 0, y2TotalMs = 0;
uint32_t lastAdc = 0, lastDs = 0, lastScan = 0, lastStats = 0, wifiLostAt = 0;
bool     wifiDown = false;

volatile bool mountEvent = false, umountEvent = false;

// --- this unit: identity, saved configuration, how it is connected --------------------------------

enum Mode { M_NORMAL, M_SETUP, M_FALLBACK, M_RECOVERY, M_NO_ID };
const char *const MODE_NAMES[] = {"normal", "setup", "fallback", "recovery", "no-identity"};

const char *const CONFIG_FILE = "/unit.cfg", *const TRY_FILE = "/wifi.try", *const BOOTS_FILE = "/boots",
                  *const BACKUP_FILE = "/backup.txt";
extern "C" uint8_t _EEPROM_start;         // the flash's last sector; the factory's page is at its start

unit::Identity   identity;
unit::UnitConfig cfg;
Mode     mode = M_NORMAL;
char     hostLabel[48] = "";
char     apSsid[32] = "";
char     lastError[80] = "";
bool     fsOk = false, backupSaved = false, bootsCleared = false;
uint32_t apIdleSince = 0, rebootAt = 0, loginBlockedUntil = 0;

struct SeenNetwork { char ssid[33]; int8_t rssi; bool open; };
const int   MAX_NETWORKS = 16;
SeenNetwork networks[MAX_NETWORKS];
int         nNetworks = 0;

struct LoginSession { char token[33]; uint32_t expires; };
LoginSession sessions[4];

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
#ifdef LOG_HOST
    if (n && WiFi.status() == WL_CONNECTED) {
      udp.beginPacket(LOG_HOST, LOG_PORT);
      udp.write(line, n);
      udp.endPacket();
    }
#endif
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
    foundF[nFound] = NAN;
    memcpy(found[nFound++], a, 8);
    bus.setResolution12(a);
    Log.printf("probe %02X %02X %02X %02X %02X %02X %02X %02X\n", a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
  }
}

float probeF(const DeviceAddress rom, int index) {
  if (romsByIndex) return index < nFound ? foundF[index] : NAN;
  for (int i = 0; i < nFound; i++) if (!memcmp(found[i], rom, 8)) return foundF[i];
  return NAN;
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
    for (int i = 0; i < nFound; i++) foundF[i] = bus.readF(found[i]);
    wsup = probeF(romWsup, 0) + cfg.offWsup;
    wret = probeF(romWret, 1) + cfg.offWret;
    bus.convertAll();
  }
  bool missing = romsByIndex ? nFound < 2 : !(isfinite(probeF(romWsup, 0)) && isfinite(probeF(romWret, 1)));
  if (missing && now - lastScan >= RESCAN_MS) {
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
      asup = ntcF(rs) + cfg.offAsup;
      aret = ntcF(rr) + cfg.offAret;
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
  n = addU(b, n, len, "cal", cfg.calibrated() ? 1 : 0);
  if (n < len && WiFi.status() == WL_CONNECTED) n += snprintf(b + n, len - n, ",\"rssi\":%ld", (long)WiFi.RSSI());
  n = addU(b, n, len, "uptime_ms", now);
  n = addU(b, n, len, "reboots", SCRATCH_TOTAL);
  if (n < len) snprintf(b + n, len - n, "}");
}

void settingsJson(char *b, size_t len, uint32_t now) {
  size_t n = snprintf(b, len, "{\"job\":\"%s\",\"result\":\"%s\",\"backup\":%d", job.busy() ? "working" : "idle", job.result, backupSaved ? 1 : 0);
  if (job.haveSettings && n < len) {
    const ScbSettings &s = job.settings;
    n += snprintf(b + n, len - n, ",\"age_s\":%lu,\"cfm_max\":%u,\"rpm_min\":%u,\"rpm_max\":%u,\"cfm\":{", (unsigned long)((now - job.settingsMs) / 1000),
                  job.cfmMax, job.rpmMin, scb::RPM_MAX);
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

// --- this unit's files ----------------------------------------------------------------------------

bool saveUnit() {
  if (!fsOk) return false;
  static char b[900];
  size_t n = unit::saveConfig(cfg, b, sizeof(b));
  if (n >= sizeof(b)) return false;
  File f = LittleFS.open("/unit.tmp", "w");
  if (!f) return false;
  bool ok = f.write((const uint8_t *)b, n) == n;
  f.close();
  if (!ok) return false;
  LittleFS.remove(CONFIG_FILE);
  return LittleFS.rename("/unit.tmp", CONFIG_FILE);
}

void loadUnit() {
  if (!fsOk) return;
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) return;
  static char b[900];
  size_t n = f.read((uint8_t *)b, sizeof(b) - 1);
  f.close();
  b[n] = 0;
  if (!unit::loadConfig(b, cfg)) Log.println("saved configuration unreadable; starting clean");
}

void randomHex(char *out, size_t bytes) {
  uint8_t r[32];
  for (size_t i = 0; i < bytes && i < sizeof(r); i += 4) {
    uint32_t v = rp2040.hwrand32();
    memcpy(r + i, &v, min((size_t)4, bytes - i));
  }
  unit::toHex(r, bytes, out);
}

void setAdminPassword(const char *pw) {
  randomHex(cfg.salt, unit::SALT_HEX / 2);
  unit::hashPassword(pw, cfg.salt, cfg.hash);
}

void useSavedProbes() {
  if (!romIsZero(cfg.romSup) && !romIsZero(cfg.romRet)) {
    memcpy(romWsup, cfg.romSup, 8);
    memcpy(romWret, cfg.romRet, 8);
  }
  romsByIndex = romIsZero(romWsup) || romIsZero(romWret);
}

// --- getting onto a network -----------------------------------------------------------------------

bool joinWifi(const char *ssid, const char *pass) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostLabel);
  WiFi.begin(ssid, pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_JOIN_MS) delay(100);
  bool ok = WiFi.status() == WL_CONNECTED;
  Log.printf("join %s: %s, %s, RSSI %d dBm, %lu ms\n", ssid, ok ? "ok" : "failed", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
             (unsigned long)(millis() - t0));
  return ok;
}

void scanNetworks() {
  nNetworks = 0;
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    const char *ssid = WiFi.SSID(i);
    if (!ssid || !*ssid || strlen(ssid) > 32) continue;
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    int at = -1;
    for (int k = 0; k < nNetworks; k++) if (!strcmp(networks[k].ssid, ssid)) at = k;
    if (at >= 0) {                                 // the same network from another access point
      if (rssi > networks[at].rssi) networks[at].rssi = rssi;
      continue;
    }
    if (nNetworks == MAX_NETWORKS) {               // keep the strongest
      int weakest = 0;
      for (int k = 1; k < nNetworks; k++) if (networks[k].rssi < networks[weakest].rssi) weakest = k;
      if (rssi <= networks[weakest].rssi) continue;
      at = weakest;
    } else {
      at = nNetworks++;
    }
    strcpy(networks[at].ssid, ssid);
    networks[at].rssi = rssi;
    networks[at].open = WiFi.encryptionType(i) == ENC_TYPE_NONE;
  }
  WiFi.scanDelete();
}

void startSetupNetwork(Mode m) {
  mode = m;
  scanNetworks();                                  // before the AP: the radio cannot do both reliably
  WiFi.end();
  snprintf(apSsid, sizeof(apSsid), "AirMaster-%s", identity.unitId);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 42, 1), IPAddress(192, 168, 42, 1), IPAddress(255, 255, 255, 0));
  WiFi.beginAP(apSsid, identity.setupPass);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", IPAddress(192, 168, 42, 1));  // every name resolves here, which raises the phone's setup sheet
  server.begin();
  apIdleSince = millis();
  Log.printf("setup network %s up, %d networks seen, mode %s\n", apSsid, nNetworks, MODE_NAMES[mode]);
}

// Counts power-ups that lasted under BOOT_WINDOW_MS. A watchdog or software reboot does not count.
bool countPowerUp() {
  if (!fsOk) return false;
  int boots = 0;
  File f = LittleFS.open(BOOTS_FILE, "r");
  if (f) {
    boots = f.read();
    f.close();
    if (boots < 0) boots = 0;
  }
  boots++;
  if (boots >= RECOVERY_BOOTS) {
    LittleFS.remove(BOOTS_FILE);
    return true;
  }
  f = LittleFS.open(BOOTS_FILE, "w");
  if (f) {
    f.write((uint8_t)boots);
    f.close();
  }
  return false;
}

void startNetwork(bool powerOn) {
  fsOk = LittleFS.begin();
  if (!fsOk) Log.println("filesystem failed; nothing can be saved");
  identity = unit::parseIdentity(&_EEPROM_start);
  loadUnit();
  backupSaved = fsOk && LittleFS.exists(BACKUP_FILE);
#ifndef HOSTNAME
#define HOSTNAME ""
#endif
  unit::hostName(cfg.name[0] ? cfg.name : HOSTNAME, identity.valid ? identity.unitId : "bench", hostLabel, sizeof(hostLabel));

  bool recovery = powerOn && countPowerUp();
  if (!powerOn) bootsCleared = true;
  if (recovery && identity.valid) {
    startSetupNetwork(M_RECOVERY);
    return;
  }

  // A new WiFi from the setup page is proved here, and only then saved.
  unit::UnitConfig trial = cfg;
  bool trying = false;
  if (fsOk && LittleFS.exists(TRY_FILE)) {
    File f = LittleFS.open(TRY_FILE, "r");
    static char b[320];                            // every byte of both fields as %XX
    size_t n = f ? f.read((uint8_t *)b, sizeof(b) - 1) : 0;
    if (f) f.close();
    b[n] = 0;
    LittleFS.remove(TRY_FILE);
    trying = unit::formGet(b, "ssid", trial.ssid, sizeof(trial.ssid)) && unit::formGet(b, "pass", trial.pass, sizeof(trial.pass)) && trial.ssid[0];
  }
  if (trying) {
    if (joinWifi(trial.ssid, trial.pass)) {
      strcpy(cfg.ssid, trial.ssid);
      strcpy(cfg.pass, trial.pass);
      saveUnit();
    } else {
      snprintf(lastError, sizeof(lastError), "Could not join %.32s. Check the password.", trial.ssid);
      WiFi.disconnect();
    }
  }
  if (WiFi.status() != WL_CONNECTED && cfg.hasWifi()) joinWifi(cfg.ssid, cfg.pass);
#ifdef SECRET_SSID
  if (WiFi.status() != WL_CONNECTED && !cfg.hasWifi()) joinWifi(SECRET_SSID, SECRET_PASS);
#endif
#ifdef SET_TOKEN
  if (!cfg.hasAdmin() && strlen(SET_TOKEN) >= 8) setAdminPassword(SET_TOKEN);
#endif

  if (WiFi.status() == WL_CONNECTED) {
    mode = M_NORMAL;
    server.begin();
    if (MDNS.begin(hostLabel)) MDNS.addService("http", "tcp", 80);
    Log.printf("boot: %s %s, %s, reboots %lu, stalls %lu\n", hostLabel, WiFi.localIP().toString().c_str(), identity.valid ? identity.serial : "no identity",
               (unsigned long)SCRATCH_TOTAL, (unsigned long)SCRATCH_STALLS);
  } else if (identity.valid) {
    startSetupNetwork(cfg.hasWifi() ? M_FALLBACK : M_SETUP);
  } else if (cfg.hasWifi()) {
    mode = M_NORMAL;                               // a bench board: keep retrying through the reboot below
    server.begin();
  } else {
    mode = M_NO_ID;
    Log.println("no factory identity and no WiFi: run AirMaster/factory on this board");
  }
}

void serviceNetwork(uint32_t now) {
  if (rebootAt && (int32_t)(now - rebootAt) >= 0) rebootNow("requested", false);
  if (!bootsCleared && now > BOOT_WINDOW_MS) {     // a power-up that lasted: the count starts again
    bootsCleared = true;
    if (fsOk) LittleFS.remove(BOOTS_FILE);
  }
  if (job.haveSettings && !backupSaved && fsOk) {  // the first time the unit sees them, before any change of its own
    static char form[420];
    size_t n = scb::settingsForm(job.settings, form, sizeof(form));
    File f = n < sizeof(form) ? LittleFS.open(BACKUP_FILE, "w") : File();
    if (f) {
      f.write((const uint8_t *)form, n);
      f.close();
      backupSaved = true;
      Log.println("original settings saved");
    }
  }
  switch (mode) {
    case M_NORMAL:
      MDNS.update();
      if (WiFi.status() == WL_CONNECTED) {
        wifiDown = false;
      } else if (!wifiDown) {
        wifiDown = true;
        wifiLostAt = now;
      } else if (now - wifiLostAt > WIFI_LOST_MS) {
        rebootNow("WiFi lost", false);
      }
      break;
    case M_SETUP:
      dns.processNextRequest();
      break;
    case M_FALLBACK:
    case M_RECOVERY:
      dns.processNextRequest();
      if (WiFi.softAPgetStationNum() > 0) apIdleSince = now;
      else if (now - apIdleSince > AP_IDLE_MS) rebootNow("setup network idle", false);
      break;
    case M_NO_ID:
      break;
  }
}

// --- login ----------------------------------------------------------------------------------------

LoginSession *findSession(const char *token, uint32_t now) {
  if (!token[0]) return nullptr;
  for (LoginSession &s : sessions) {
    if (s.token[0] && (int32_t)(s.expires - now) > 0 && unit::sameText(s.token, token)) return &s;
  }
  return nullptr;
}

bool loggedIn(const HttpRequest &r, uint32_t now) { return findSession(r.session, now) != nullptr; }

const char *newSession(uint32_t now) {
  LoginSession *slot = &sessions[0];
  for (LoginSession &s : sessions) {
    if (!s.token[0] || (int32_t)(s.expires - now) <= 0) { slot = &s; break; }
    if ((int32_t)(s.expires - slot->expires) < 0) slot = &s;     // full: the oldest gives way
  }
  randomHex(slot->token, 16);
  slot->expires = now + SESSION_MS;
  return slot->token;
}

void clearSessions() {
  for (LoginSession &s : sessions) s.token[0] = 0;
}

// --- answers --------------------------------------------------------------------------------------

const char *statusText(int code) {
  switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 413: return "Too Large";
    case 429: return "Too Many Requests";
    default:  return "Error";
  }
}

void respond(WiFiClient &c, int code, const char *body, const char *extraHeader = "") {
  c.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nConnection: close\r\n%s\r\n", code, statusText(code), extraHeader);
  c.println(body);
}

void servePage(WiFiClient &c) {
  c.print("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\nCache-Control: no-cache\r\nConnection: close\r\n");
  c.printf("Content-Length: %u\r\n\r\n", (unsigned)WEB_PAGE_GZ_LEN);
  size_t sent = 0;
  uint32_t last = millis();
  while (sent < WEB_PAGE_GZ_LEN && c.connected() && millis() - last < HTTP_TIMEOUT_MS) {
    rp2040.wdt_reset();
    serviceUsb(millis());
    size_t n = WEB_PAGE_GZ_LEN - sent;
    if (n > 1024) n = 1024;
    size_t w = c.write(WEB_PAGE_GZ + sent, n);
    if (w) {
      sent += w;
      last = millis();
    }
  }
}

size_t ap(char *b, size_t n, size_t len, const char *fmt, ...) {
  if (n >= len) return n;
  va_list args;
  va_start(args, fmt);
  int w = vsnprintf(b + n, len - n, fmt, args);
  va_end(args);
  if (w < 0) return n;
  return n + (size_t)w < len ? n + (size_t)w : len - 1;
}

size_t apStr(char *b, size_t n, size_t len, const char *key, const char *val) {
  n = ap(b, n, len, "\"%s\":\"", key);
  n = unit::jsonStr(b, n, len, val);
  return ap(b, n, len, "\",");
}

void infoJson(char *b, size_t len, const HttpRequest &r, uint32_t now) {
  size_t n = ap(b, 0, len, "{");
  n = apStr(b, n, len, "mode", MODE_NAMES[mode]);
  n = apStr(b, n, len, "fw", FW_VERSION);
  n = apStr(b, n, len, "serial", identity.serial);
  n = apStr(b, n, len, "name", cfg.name);
  n = apStr(b, n, len, "host", hostLabel);
  n = apStr(b, n, len, "ap", apSsid);
  n = apStr(b, n, len, "ssid", WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "");
  n = apStr(b, n, len, "ip", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
  n = apStr(b, n, len, "error", lastError);
  n = ap(b, n, len, "\"admin\":%d,\"wifi\":%d,\"logged_in\":%d,\"cal\":%d,\"by_order\":%d,\"backup\":%d,", cfg.hasAdmin() ? 1 : 0, cfg.hasWifi() ? 1 : 0,
         loggedIn(r, now) ? 1 : 0, cfg.calibrated() ? 1 : 0, romsByIndex ? 1 : 0, backupSaved ? 1 : 0);
  n = ap(b, n, len, "\"offsets\":{\"wsup\":%.3f,\"wret\":%.3f,\"asup\":%.3f,\"aret\":%.3f},\"probes\":[", cfg.offWsup, cfg.offWret, cfg.offAsup, cfg.offAret);
  for (int i = 0; i < nFound; i++) {
    char rom[17];
    unit::toHex(found[i], 8, rom);
    const char *role = romsByIndex ? (i == 0 ? "sup" : i == 1 ? "ret" : "") : !memcmp(found[i], romWsup, 8) ? "sup" : !memcmp(found[i], romWret, 8) ? "ret" : "";
    n = ap(b, n, len, "%s{\"rom\":\"%s\",\"role\":\"%s\"", i ? "," : "", rom, role);
    if (isfinite(foundF[i])) n = ap(b, n, len, ",\"f\":%.2f", foundF[i]);
    n = ap(b, n, len, "}");
  }
  n = ap(b, n, len, "],\"networks\":[");
  if (mode != M_NORMAL) {
    for (int i = 0; i < nNetworks; i++) {
      n = ap(b, n, len, "%s{\"ssid\":\"", i ? "," : "");
      n = unit::jsonStr(b, n, len, networks[i].ssid);
      n = ap(b, n, len, "\",\"rssi\":%d,\"open\":%d}", networks[i].rssi, networks[i].open ? 1 : 0);
    }
  }
  ap(b, n, len, "]}");
}

int jobError(char *body, size_t len, int code, const char *msg) {
  snprintf(body, len, "{\"error\":\"%s\"}", msg);
  return code;
}

// Starts a read (form null) or a change. The form is decoded in place.
int startJob(char *form, char *body, size_t len, uint32_t now) {
  if (job.busy()) return jobError(body, len, 409, "a job is running");
  job.clear();
  if (form) {
    size_t added = 0;
    for (char *p = form, *next; p && *p; p = next) {
      next = strchr(p, '&');
      if (next) *next++ = 0;
      char *eq = strchr(p, '=');
      if (!eq) return jobError(body, len, 400, "malformed form");
      *eq = 0;
      unit::urlDecode(p);
      unit::urlDecode(eq + 1);
      if (!job.add(p, eq + 1)) {
        snprintf(body, len, "{\"error\":\"bad setting %s\"}", p);
        return 400;
      }
      added++;
    }
    if (!added) return jobError(body, len, 400, "no settings given");
  }
  if (!job.begin(now, SerialHost.mounted() && haveFrame && now - lastFrameMs < SCB_REPORT_MS)) return jobError(body, len, 503, job.result);
  Log.printf("job started: %s\n", form ? "set" : "read");
  snprintf(body, len, "{\"job\":\"started\"}");
  return 202;
}

bool validPassword(const char *pw) { return strlen(pw) >= 8; }

// The first-setup form, and the same form on recovery or fallback, where a blank field keeps what is saved.
int postSetup(const char *form, char *body, size_t len, uint32_t now) {
  if (mode == M_NORMAL) return jobError(body, len, 403, "setup is only open on the unit's own network");
  char ssid[33] = "", pass[64] = "", name[33] = "", admin[64] = "";
  bool hasSsid = unit::formGet(form, "ssid", ssid, sizeof(ssid)) && ssid[0];
  unit::formGet(form, "pass", pass, sizeof(pass));
  bool hasName = unit::formGet(form, "name", name, sizeof(name));
  bool hasAdmin = unit::formGet(form, "admin", admin, sizeof(admin)) && admin[0];
  if (!hasSsid && !cfg.hasWifi()) return jobError(body, len, 400, "Choose a WiFi network.");
  if (!hasAdmin && !cfg.hasAdmin()) return jobError(body, len, 400, "Set an admin password.");
  if (hasAdmin && !validPassword(admin)) return jobError(body, len, 400, "The admin password needs at least 8 characters.");
  if (pass[0] && strlen(pass) < 8) return jobError(body, len, 400, "A WiFi password has at least 8 characters.");
  if (hasName && name[0]) strcpy(cfg.name, name);
  if (hasAdmin) {
    setAdminPassword(admin);
    clearSessions();
  }
  if (!saveUnit()) return jobError(body, len, 500, "could not save");
  if (hasSsid) {
    File f = LittleFS.open(TRY_FILE, "w");           // saved for good only once the join works, in startNetwork()
    if (!f) return jobError(body, len, 500, "could not save");
    f.print("ssid=");
    for (const char *p = ssid; *p; p++) f.printf("%%%02X", (unsigned char)*p);
    f.print("&pass=");
    for (const char *p = pass; *p; p++) f.printf("%%%02X", (unsigned char)*p);
    f.close();
  }
  char label[48];
  unit::hostName(cfg.name, identity.unitId, label, sizeof(label));
  size_t n = ap(body, 0, len, "{");
  n = apStr(body, n, len, "host", label);
  n = apStr(body, n, len, "ap", apSsid);
  ap(body, n, len, "\"ok\":1}");
  rebootAt = now + 1500;
  if (!rebootAt) rebootAt = 1;
  return 200;
}

// Name, admin password, sensor roles and offsets, each only when present.
int postUnit(const char *form, char *body, size_t len) {
  char v[64];
  if (unit::formGet(form, "name", v, 33) && v[0]) strcpy(cfg.name, v);
  if (unit::formGet(form, "admin", v, sizeof(v)) && v[0]) {
    if (!validPassword(v)) return jobError(body, len, 400, "The admin password needs at least 8 characters.");
    setAdminPassword(v);
  }
  char sup[20], ret[20];
  if (unit::formGet(form, "romsup", sup, sizeof(sup)) && unit::formGet(form, "romret", ret, sizeof(ret))) {
    uint8_t a[8], b[8];
    if (unit::fromHex(sup, a, 8) != 8 || unit::fromHex(ret, b, 8) != 8 || !memcmp(a, b, 8)) return jobError(body, len, 400, "Choose two different probes.");
    memcpy(cfg.romSup, a, 8);
    memcpy(cfg.romRet, b, 8);
    useSavedProbes();
  }
  struct { const char *k; float *p; } offs[] = {{"offwsup", &cfg.offWsup}, {"offwret", &cfg.offWret}, {"offasup", &cfg.offAsup}, {"offaret", &cfg.offAret}};
  for (auto &o : offs) {
    if (!unit::formGet(form, o.k, v, sizeof(v))) continue;
    char *end;
    float f = strtof(v, &end);
    if (end == v || *end || !isfinite(f) || fabsf(f) > 20) return jobError(body, len, 400, "Offsets are degrees F, within 20.");
    *o.p = f;
  }
  if (!saveUnit()) return jobError(body, len, 500, "could not save");
  snprintf(body, len, "{\"ok\":1}");
  return 200;
}

// Streams the firmware to the filesystem, which the bootloader copies into place on the next boot.
void receiveUpdate(WiFiClient &c, const HttpRequest &r, char *body, size_t len) {
  if (job.busy()) { respond(c, jobError(body, len, 409, "a job is running"), body); return; }
  if (!r.contentLength || r.contentLength > FIRMWARE_MAX) { respond(c, jobError(body, len, 413, "That is not an AirMaster firmware file."), body); return; }
  if (!Update.begin(r.contentLength)) { respond(c, jobError(body, len, 500, "not enough room for the update"), body); return; }
  static uint8_t buf[1024];
  size_t got = 0;
  uint32_t last = millis();
  while (got < r.contentLength && c.connected() && millis() - last < 15000) {
    rp2040.wdt_reset();
    serviceUsb(millis());
    int a = c.available();
    if (a <= 0) continue;
    size_t want = min((size_t)a, sizeof(buf));
    if (want > r.contentLength - got) want = r.contentLength - got;
    int n = c.read(buf, want);
    if (n <= 0) continue;
    if (Update.write(buf, n) != (size_t)n) break;
    got += n;
    last = millis();
  }
  if (got != r.contentLength || !Update.end()) {
    Update.end(true);
    Log.printf("update failed after %u of %u bytes\n", (unsigned)got, (unsigned)r.contentLength);
    respond(c, jobError(body, len, 400, "The update did not complete. The unit is still on its old firmware."), body);
    return;
  }
  snprintf(body, len, "{\"ok\":1}");
  respond(c, 200, body);
  rebootAt = millis() + 1500;
}

int route(WiFiClient &c, const HttpRequest &r, char *form, char *body, size_t len, uint32_t now, const char **extra) {
  static char cookie[100];
  const char *t = r.target;
  bool get = !strcmp(r.method, "GET"), post = !strcmp(r.method, "POST");
  if (get) {
    if (!strcmp(t, "/data") || (!strcmp(t, "/") && !r.wantsHtml)) { buildLine(body, len, now); return 200; }
    if (!strcmp(t, "/settings")) { settingsJson(body, len, now); return 200; }
    if (!strcmp(t, "/api/info")) { infoJson(body, len, r, now); return 200; }
    if (!strcmp(t, "/")) return 0;                 // the page
    return jobError(body, len, 404, "not found");
  }
  if (!post) return jobError(body, len, 400, "GET or POST only");
  if (!r.markedApi) return jobError(body, len, 403, "use the AirMaster page");

  if (!strcmp(t, "/api/setup")) return postSetup(form, body, len, now);
  if (!strcmp(t, "/api/login")) {
    if ((int32_t)(loginBlockedUntil - now) > 0) return jobError(body, len, 429, "Wait a moment and try again.");
    char pw[64] = "", hash[unit::HASH_HEX + 1];
    unit::formGet(form, "password", pw, sizeof(pw));
    if (!cfg.hasAdmin()) return jobError(body, len, 401, "No admin password is set.");
    rp2040.wdt_reset();
    unit::hashPassword(pw, cfg.salt, hash);
    if (!unit::sameText(hash, cfg.hash)) {
      loginBlockedUntil = millis() + LOGIN_DELAY_MS;
      return jobError(body, len, 401, "Wrong password.");
    }
    snprintf(cookie, sizeof(cookie), "Set-Cookie: s=%s; Path=/; Max-Age=43200; HttpOnly; SameSite=Strict\r\n", newSession(now));
    *extra = cookie;
    snprintf(body, len, "{\"ok\":1}");
    return 200;
  }
  if (!loggedIn(r, now)) return jobError(body, len, 401, "Log in first.");

  if (!strcmp(t, "/api/logout")) {
    LoginSession *s = findSession(r.session, now);
    if (s) s->token[0] = 0;
    snprintf(body, len, "{\"ok\":1}");
    return 200;
  }
  if (!strcmp(t, "/api/unit")) return postUnit(form, body, len);
  if (!strcmp(t, "/api/settings/read")) return startJob(nullptr, body, len, now);
  if (!strcmp(t, "/api/set")) {
    if (!backupSaved) return jobError(body, len, 409, "read_first");   // never a change before the originals are saved
    return startJob(form, body, len, now);
  }
  if (!strcmp(t, "/api/restore")) {
    File f = fsOk ? LittleFS.open(BACKUP_FILE, "r") : File();
    if (!f) return jobError(body, len, 404, "No original settings are saved.");
    size_t n = f.read((uint8_t *)form, FORM_MAX);
    f.close();
    form[n] = 0;
    return startJob(form, body, len, now);
  }
  if (!strcmp(t, "/api/reboot")) {
    rebootAt = now + 1000;
    snprintf(body, len, "{\"ok\":1}");
    return 200;
  }
  if (!strcmp(t, "/api/factory-reset")) {
    if (fsOk) {
      LittleFS.remove(CONFIG_FILE);
      LittleFS.remove(TRY_FILE);
      LittleFS.remove(BACKUP_FILE);
    }
    clearSessions();
    rebootAt = now + 1000;
    snprintf(body, len, "{\"ok\":1}");
    return 200;
  }
  return jobError(body, len, 404, "not found");
}

void serveClient() {
  WiFiClient client = server.accept();
  if (!client) return;
  static HttpRequest r;
  r = HttpRequest();
  static char body[3072];
  static char form[FORM_MAX + 1];
  uint32_t start = millis();
  bool done = false;
  while (!done && client.connected() && millis() - start < HTTP_TIMEOUT_MS) {
    serviceUsb(millis());                        // frames keep arriving while a slow client talks
    if (!client.available()) continue;
    done = r.feed(client.read());
  }
  if (!done) {
    client.stop();
    return;
  }
  uint32_t now = millis();
  bool ap_ = mode != M_NORMAL;
  if (r.tooLong) {
    respond(client, jobError(body, sizeof(body), 400, "request too long"), body);
  } else if (ap_ && !strcmp(r.method, "GET") && strcmp(r.host, "192.168.42.1")) {
    // A phone's check that it is online, or anything else on the setup network: send it to the setup page.
    client.print("HTTP/1.1 302 Found\r\nLocation: http://192.168.42.1/\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  } else if (!strcmp(r.method, "POST") && !strcmp(r.target, "/api/update")) {
    if (!r.markedApi || !loggedIn(r, now)) respond(client, jobError(body, sizeof(body), 401, "Log in first."), body);
    else receiveUpdate(client, r, body, sizeof(body));
  } else {
    size_t n = 0;
    if (r.contentLength > FORM_MAX) {
      respond(client, jobError(body, sizeof(body), 413, "form too long"), body);
      client.stop();
      return;
    }
    start = millis();
    while (n < r.contentLength && client.connected() && millis() - start < HTTP_TIMEOUT_MS) {
      serviceUsb(millis());
      int a = client.available();
      if (a > 0) form[n++] = client.read();
    }
    form[n] = 0;
    const char *extra = "";
    int code = route(client, r, form, body, sizeof(body), now, &extra);
    if (code == 0) servePage(client);
    else respond(client, code, body, extra);
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
    SCRATCH_POWER_ON = 1;
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

  bool powerOn = SCRATCH_POWER_ON;
  SCRATCH_POWER_ON = 0;
  startNetwork(powerOn);

  bool ds = bus.begin();
  Log.println(ds ? "DS2482 found" : "DS2482 did not answer at 0x18, or failed its reset: check 3.3 V at U2 pins 1 and 3");
  useSavedProbes();
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

  serviceNetwork(now);

  serviceUsb(now);
  job.tick(now, parser);
  logJob();
  serviceAdc(now);
  serviceDs(now);
  showLeds(now);
  if (mode != M_NO_ID) serveClient();
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
