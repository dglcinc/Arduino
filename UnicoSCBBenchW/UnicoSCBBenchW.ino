// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// UnicoSCBBenchW: the Unico SCB over the Pico W's built-in USB host, with WiFi running.
//
// Board: Raspberry Pi Pico W or WH. The Unico USB board plugs into the Pico's micro-USB port through
// an OTG adapter, and 5 V goes in on pin 40 (VBUS), pin 38 ground. Listen only, as UnicoSCBBench:
// nothing here writes to SerialHost. The native port is the host, so there is no USB console: every
// line goes by UDP to LOG_HOST:LOG_PORT and to Serial1 (GP0).
//
// The sketch reboots itself when no status frame has arrived for SILENCE_MS, and when no device has
// mounted within MOUNT_MS of boot, because boot is the path the Feather bench run showed to work.
//
// Build: arduino-pico core, board "Raspberry Pi Pico W", USB Stack "Adafruit TinyUSB Host (native)":
//   arduino-cli compile -b rp2040:rp2040:rpipicow:usbstack=tinyusb_host UnicoSCBBenchW
// Needs arduino_secrets.h (SECRET_SSID, SECRET_PASS) in this folder, from arduino_secrets.h.example.
// To reflash, hold BOOTSEL and plug the Pico into a computer.

#include <WiFi.h>
#include <WiFiUdp.h>
#include "arduino_secrets.h"
#include "Adafruit_TinyUSB.h"

#define LOG_HOST         "10.0.0.84"
#define LOG_PORT         5515
#define SILENCE_MS       20000UL
#define MOUNT_MS         30000UL

WiFiUDP udp;

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
  uint8_t line[256];
  size_t n = 0;
};
UdpLog Log;



#define SCB_BAUD         4800
#define STATS_MS         60000UL
#define STALE_MS         3000UL     // four missed frames

#define STATUS_ID        0x83
#define LOG_ID           0x84
#define PC_TAG           0x1C       // byte 3 of every PC frame

Adafruit_USBH_Host USBHost;
Adafruit_USBH_CDC  SerialHost;

static const char *MODES[6] = {"G", "Y1", "Y2", "W1", "W2", "E"};

uint8_t  buf[1024];
size_t   nBuf = 0;

bool     rawDump = false;
uint32_t nStatus = 0, nLog = 0, nPc = 0, busBytes = 0, overflows = 0;
uint32_t lastFrameMs = 0, gapMin = 0xFFFFFFFF, gapMax = 0;
uint32_t lastStatsMs = 0;
bool     staleWarned = false;
uint16_t lastStored[12];
bool     haveStored = false;

volatile bool mountEvent = false, umountEvent = false;
volatile uint16_t usbVid = 0, usbPid = 0;

// Any device that finishes enumeration, whether or not the CDC driver binds to it.
volatile bool devEvent = false, devGoneEvent = false, descEvent = false, descFailed = false;
volatile uint16_t devVid = 0, devPid = 0;
volatile uint8_t devAddr = 0;
uint8_t descBuf[256];

// --- framing ------------------------------------------------------------------------------------

bool fletcherOk(const uint8_t *f, size_t len) {
  uint16_t s1 = 0, s2 = 0;
  for (size_t i = 0; i < len; i++) {
    s1 = (s1 + f[i]) % 255;
    s2 = (s2 + s1) % 255;
  }
  return s1 == 0 && s2 == 0 && f[len - 1] != 0 && f[len - 2] != 0;
}

uint16_t u16(const uint8_t *f, size_t i) { return (uint16_t(f[i]) << 8) | f[i + 1]; }

void printHex(const uint8_t *f, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (f[i] < 16) Log.print('0');
    Log.print(f[i], HEX);
    Log.print(i + 1 < len ? ' ' : '\n');
  }
}

// decode_status() for the 9-, 36-, 37- and 51-byte SCB frames
void onStatus(const uint8_t *f, size_t len) {
  uint32_t now = millis();
  uint32_t gap = lastFrameMs ? now - lastFrameMs : 0;
  if (lastFrameMs) {
    if (gap < gapMin) gapMin = gap;
    if (gap > gapMax) gapMax = gap;
  }
  lastFrameMs = now;
  staleWarned = false;
  nStatus++;
  digitalWrite(LED_BUILTIN, nStatus & 1);

  uint8_t calls = f[4] >> 2;                 // active-low G Y1 Y2 W1 W2 E, bit 5..0
  char callText[24] = "";
  for (int k = 0; k < 6; k++) {
    if (!((calls >> (5 - k)) & 1)) {
      if (callText[0]) strcat(callText, ",");
      strcat(callText, MODES[k]);
    }
  }
  if (!callText[0]) strcpy(callText, "-");

  static const char *CHASSIS[4] = {"1218", "2430", "3642", "4860"};
  const char *model = CHASSIS[f[4] & 3];
  Log.printf("#%lu t=%.1fs ", (unsigned long)nStatus, now / 1000.0f);

  if (len == 9) {
    uint16_t sw = u16(f, 5);
    Log.printf("%s len=9 calls=%s fw=3.%u switches=0x%04X gap=%lums\n",
                  model, callText, f[3], sw, (unsigned long)gap);
    return;
  }

  int b = (len == 37) ? 1 : 0;
  uint16_t powerLevel = b ? u16(f, 5) : f[5];
  int p = 6 + b;
  uint16_t stored[12];
  for (int k = 0; k < 12; k++) stored[k] = u16(f, p + 2 * k);
  uint16_t actualCfm = u16(f, p + 24), actualRpm = u16(f, p + 26);

  if (len == 51) {
    uint16_t sw = u16(f, 38);
    if (powerLevel == 12 && (sw & 0x0400)) model = "3036";
    Log.printf("%s len=51 calls=%s actual=%u CFM %u RPM bus=%uV fw=3.%u switches=0x%04X motor=0x%04X gap=%lums\n",
                  model, callText, actualCfm, actualRpm, f[37] * 4, f[46], sw, u16(f, 47),
                  (unsigned long)gap);
  } else {
    Log.printf("%s len=%u flags=0x%02X calls=%s actual=%u CFM %u RPM gap=%lums\n",
                  model, (unsigned)len, f[2], callText, actualCfm, actualRpm, (unsigned long)gap);
  }

  if (!haveStored || memcmp(stored, lastStored, sizeof(stored)) != 0) {
    Log.print("    stored CFM");
    for (int k = 0; k < 6; k++) Log.printf(" %s=%u", MODES[k], stored[k]);
    Log.print("\n    RPM limit ");
    for (int k = 0; k < 6; k++) Log.printf(" %s=%u", MODES[k], stored[6 + k]);
    Log.println();
    memcpy(lastStored, stored, sizeof(stored));
    haveStored = true;
  }
}

void onFrame(const uint8_t *f, size_t len) {
  if (rawDump) printHex(f, len);
  if (f[1] == LOG_ID) {
    nLog++;
    Log.printf("log frame, %u bytes (Fresh Air Monitor)\n", (unsigned)len);
  } else if (f[3] == PC_TAG) {
    nPc++;                                    // another master answering; this sketch sends none
    Log.printf("PC frame, %u bytes: something else is answering the SCB\n", (unsigned)len);
  } else {
    onStatus(f, len);
  }
}

// split_frames(): find 01 83 / 01 84, try the lengths the byte after the header allows, and accept
// the first whose Fletcher sums are zero. Bytes that are not a frame are counted, not kept.
void parse() {
  static const uint8_t L_LOG[] = {102}, L_SHORT[] = {9}, L_PC[] = {43, 31}, L_STATUS[] = {36, 37, 51};
  size_t i = 0;
  while (i + 1 < nBuf) {
    if (buf[i] == 0x01 && (buf[i + 1] == STATUS_ID || buf[i + 1] == LOG_ID)) {
      const uint8_t *lens;
      size_t nl;
      if (buf[i + 1] == LOG_ID) {
        lens = L_LOG; nl = 1;
      } else if (i + 3 < nBuf && buf[i + 3] != 20 && buf[i + 3] != 23 && buf[i + 3] != PC_TAG) {
        lens = L_SHORT; nl = 1;
      } else if (i + 3 < nBuf && buf[i + 3] == PC_TAG) {
        lens = L_PC; nl = 2;
      } else {
        lens = L_STATUS; nl = 3;
      }
      size_t hit = 0, longest = 0;
      for (size_t k = 0; k < nl; k++) {
        if (lens[k] > longest) longest = lens[k];
        if (!hit && i + lens[k] <= nBuf && fletcherOk(buf + i, lens[k])) hit = lens[k];
      }
      if (!hit && i + longest > nBuf) break;  // wait for more bytes
      if (hit) {
        onFrame(buf + i, hit);
        i += hit;
        continue;
      }
    }
    busBytes++;
    i++;
  }
  memmove(buf, buf + i, nBuf - i);
  nBuf -= i;
}

void printStats() {
  Log.printf("stats: %lu status, %lu log, %lu PC frames, %lu stray bytes, %lu overflows, gap %lu-%lu ms, host %s at %lu baud\n",
                (unsigned long)nStatus, (unsigned long)nLog, (unsigned long)nPc,
                (unsigned long)busBytes, (unsigned long)overflows,
                (unsigned long)(gapMin == 0xFFFFFFFF ? 0 : gapMin), (unsigned long)gapMax,
                SerialHost.connected() ? "connected" : (SerialHost.mounted() ? "mounted" : "absent"),
                (unsigned long)SerialHost.baud());
  Log.printf("wifi: RSSI %d dBm, uptime %lu s\n", WiFi.RSSI(), (unsigned long)(millis() / 1000));
  gapMin = 0xFFFFFFFF;
  gapMax = 0;
}

// --- core 0: decoding and the Mac-side serial ---------------------------------------------------

void setup() {
  Serial1.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  WiFi.mode(WIFI_STA);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(100);
  Serial1.println("boot");
  Log.printf("boot: UnicoSCBBenchW, native USB host, listen only, %s, RSSI %d dBm, joined in %lu ms\n",
             WiFi.localIP().toString().c_str(), WiFi.RSSI(), millis() - t0);
  lastStatsMs = millis();
}

void loop() {
  if (mountEvent) {
    mountEvent = false;
    Log.printf("USB device mounted: VID %04X PID %04X, CDC line at %lu baud\n",
                  usbVid, usbPid, (unsigned long)SerialHost.baud());
  }
  if (umountEvent) {
    umountEvent = false;
    Log.println("USB device removed");
  }
  if (devEvent) {
    devEvent = false;
    Log.printf("enumerated: addr %u VID %04X PID %04X\n", devAddr, devVid, devPid);
  }
  if (devGoneEvent) {
    devGoneEvent = false;
    Log.println("device gone");
  }
  if (descFailed) {
    descFailed = false;
    Log.println("config descriptor read failed");
  }
  if (descEvent) {
    descEvent = false;
    uint16_t total = descBuf[2] | (descBuf[3] << 8);
    Log.printf("config descriptor, %u bytes:", total);
    for (uint16_t i = 0; i < total && i < sizeof(descBuf); i++) Log.printf(" %02X", descBuf[i]);
    Log.println();
  }

  if (SerialHost.mounted()) {
    int avail = SerialHost.available();
    if (avail > 0) {
      if (nBuf + avail > sizeof(buf)) {         // never expected at 4800 baud; drop the oldest half
        overflows++;
        memmove(buf, buf + sizeof(buf) / 2, nBuf - sizeof(buf) / 2);
        nBuf -= sizeof(buf) / 2;
      }
      size_t room = sizeof(buf) - nBuf;
      nBuf += SerialHost.read(buf + nBuf, (size_t)avail < room ? avail : room);
      parse();
    }
  }

  uint32_t now = millis();
  if (lastFrameMs && !staleWarned && now - lastFrameMs > STALE_MS) {
    Log.printf("no status frame for %lu ms\n", (unsigned long)(now - lastFrameMs));
    staleWarned = true;
  }
  if (lastFrameMs && now - lastFrameMs > SILENCE_MS) {
    Log.printf("REBOOT: no status frame for %lu ms, %s\n", (unsigned long)(now - lastFrameMs),
               SerialHost.connected() ? "host still connected" : "host not connected");
    delay(300);
    rp2040.reboot();
  }
  if (!lastFrameMs && now > MOUNT_MS) {
    Log.printf("REBOOT: no status frame %lu ms after boot, host %s\n", (unsigned long)now,
               SerialHost.mounted() ? "mounted" : "absent");
    delay(300);
    rp2040.reboot();
  }
  if (now - lastStatsMs >= STATS_MS) {
    lastStatsMs = now;
    printStats();
  }
}

// --- core 1: the USB host stack on PIO USB -----------------------------------------------------

void setup1() {
  USBHost.begin(0);                            // the RP2040's own USB controller
  SerialHost.begin(SCB_BAUD);                  // applied on mount; DTR/RTS rise on enumeration
}

void loop1() {
  USBHost.task();
}

extern "C" {

void tuh_cdc_mount_cb(uint8_t idx) {
  SerialHost.mount(idx);
  tuh_itf_info_t info;
  uint16_t vid = 0, pid = 0;
  if (tuh_cdc_itf_get_info(idx, &info)) tuh_vid_pid_get(info.daddr, &vid, &pid);
  usbVid = vid;
  usbPid = pid;
  mountEvent = true;
}

void descDone(tuh_xfer_t *xfer) {
  if (xfer->result == XFER_RESULT_SUCCESS) descEvent = true;
  else descFailed = true;
}

void tuh_mount_cb(uint8_t addr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(addr, &vid, &pid);
  devVid = vid;
  devPid = pid;
  devAddr = addr;
  devEvent = true;
  tuh_descriptor_get_configuration(addr, 0, descBuf, sizeof(descBuf), descDone, 0);
}

void tuh_umount_cb(uint8_t addr) {
  (void)addr;
  devGoneEvent = true;
}

void tuh_cdc_umount_cb(uint8_t idx) {
  SerialHost.umount(idx);
  umountEvent = true;
}

}
