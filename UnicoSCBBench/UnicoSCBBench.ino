// UnicoSCBBench — bench test of the USB host route to a Unico S.M.A.R.T. Control Board (SCB).
//
// Board: Adafruit Feather RP2040 USB Host (product 5723). The Unico USB board in the air handler's
// control box plugs into the Feather's USB-A jack; the Feather's USB-C goes to the Mac. Open the
// Serial Monitor at 115200. This proves that an RP2040 can host the SCB's Cypress CDC board, which
// is the premise of the air-handler node's rev B Pico WH carrier (pivac
// docs/mbr-air-handler-node-shield-review.md §0, handoff of 2026-10-24).
//
// Listen only. The sketch never writes to SerialHost: the SCB treats every PC frame as a write
// request (pivac docs/unico-scb-protocol.md §5), so nothing here can change a setting. The only
// traffic toward the USB board is enumeration, the 4800 8N1 line coding and DTR/RTS, which is what
// macOS does when pyserial opens the port.
//
// The SCB is bus master and sends a 36-byte status frame every 0.85 s whether or not anything
// answers. Each good frame prints one line:
//
//   #42 t=36.1s 2430 len=36 calls=G,Y1 actual=300 CFM 1078 RPM gap=850ms
//
// and the stored airflows and RPM limits print again whenever they change. Every 30 s a stats line
// gives frame counts, bytes that were not a frame, and the shortest and longest gap between status
// frames. Type `r` to toggle a hex dump of every frame (pastes into `scripts/unico-scb.py`'s raw
// format), `s` for stats now.
//
// Framing and decoding are ports of pivac/unico_scb_protocol.py (split_frames, decode_status).
//
// Build: arduino-pico core (earlephilhower), board "Adafruit Feather RP2040 USB Host",
// USB Stack "Adafruit TinyUSB", CPU Speed 120 MHz (PIO USB needs a multiple of 12 MHz):
//   arduino-cli compile -b rp2040:rp2040:adafruit_feather_usb_host:usbstack=tinyusb,freq=120 UnicoSCBBench
// The core bundles Adafruit TinyUSB; Pico-PIO-USB is a library: arduino-cli lib install "Pico PIO USB".
// On a Pico W or Pico WH the PIO state machines and DMA channel must move off the CYW43's (see the
// ARDUINO_RASPBERRY_PI_PICO_W block in TinyUSB's DualRole examples' usbh_helper.h); the Feather
// needs none of that.

#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"

#ifndef PIN_USB_HOST_DP
#define PIN_USB_HOST_DP  16
#endif
#ifndef PIN_5V_EN
#define PIN_5V_EN        18
#endif
#ifndef PIN_5V_EN_STATE
#define PIN_5V_EN_STATE  1
#endif

#define SCB_BAUD         4800
#define STATS_MS         30000UL
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
    if (f[i] < 16) Serial.print('0');
    Serial.print(f[i], HEX);
    Serial.print(i + 1 < len ? ' ' : '\n');
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
  Serial.printf("#%lu t=%.1fs ", (unsigned long)nStatus, now / 1000.0f);

  if (len == 9) {
    uint16_t sw = u16(f, 5);
    Serial.printf("%s len=9 calls=%s fw=3.%u switches=0x%04X gap=%lums\n",
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
    Serial.printf("%s len=51 calls=%s actual=%u CFM %u RPM bus=%uV fw=3.%u switches=0x%04X motor=0x%04X gap=%lums\n",
                  model, callText, actualCfm, actualRpm, f[37] * 4, f[46], sw, u16(f, 47),
                  (unsigned long)gap);
  } else {
    Serial.printf("%s len=%u flags=0x%02X calls=%s actual=%u CFM %u RPM gap=%lums\n",
                  model, (unsigned)len, f[2], callText, actualCfm, actualRpm, (unsigned long)gap);
  }

  if (!haveStored || memcmp(stored, lastStored, sizeof(stored)) != 0) {
    Serial.print("    stored CFM");
    for (int k = 0; k < 6; k++) Serial.printf(" %s=%u", MODES[k], stored[k]);
    Serial.print("\n    RPM limit ");
    for (int k = 0; k < 6; k++) Serial.printf(" %s=%u", MODES[k], stored[6 + k]);
    Serial.println();
    memcpy(lastStored, stored, sizeof(stored));
    haveStored = true;
  }
}

void onFrame(const uint8_t *f, size_t len) {
  if (rawDump) printHex(f, len);
  if (f[1] == LOG_ID) {
    nLog++;
    Serial.printf("log frame, %u bytes (Fresh Air Monitor)\n", (unsigned)len);
  } else if (f[3] == PC_TAG) {
    nPc++;                                    // another master answering; this sketch sends none
    Serial.printf("PC frame, %u bytes: something else is answering the SCB\n", (unsigned)len);
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
  Serial.printf("stats: %lu status, %lu log, %lu PC frames, %lu stray bytes, %lu overflows, gap %lu-%lu ms, host %s at %lu baud\n",
                (unsigned long)nStatus, (unsigned long)nLog, (unsigned long)nPc,
                (unsigned long)busBytes, (unsigned long)overflows,
                (unsigned long)(gapMin == 0xFFFFFFFF ? 0 : gapMin), (unsigned long)gapMax,
                SerialHost.connected() ? "connected" : (SerialHost.mounted() ? "mounted" : "absent"),
                (unsigned long)SerialHost.baud());
  gapMin = 0xFFFFFFFF;
  gapMax = 0;
}

// --- core 0: decoding and the Mac-side serial ---------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("UnicoSCBBench: listen only, 4800 8N1 on the USB-A host port. r = hex dump, s = stats");
}

void loop() {
  if (mountEvent) {
    mountEvent = false;
    Serial.printf("USB device mounted: VID %04X PID %04X, CDC line at %lu baud\n",
                  usbVid, usbPid, (unsigned long)SerialHost.baud());
  }
  if (umountEvent) {
    umountEvent = false;
    Serial.println("USB device removed");
  }

  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'r') {
      rawDump = !rawDump;
      Serial.printf("hex dump %s\n", rawDump ? "on" : "off");
    } else if (c == 's') {
      printStats();
    }
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
    Serial.printf("no status frame for %lu ms\n", (unsigned long)(now - lastFrameMs));
    staleWarned = true;
  }
  if (now - lastStatsMs >= STATS_MS) {
    lastStatsMs = now;
    printStats();
  }
}

// --- core 1: the USB host stack on PIO USB -----------------------------------------------------

void setup1() {
  uint32_t cpuHz = clock_get_hz(clk_sys);
  if (cpuHz % 12000000UL) {
    while (true) {
      Serial.printf("CPU clock %lu Hz: PIO USB needs a multiple of 12 MHz; set CPU Speed to 120 MHz\n",
                    (unsigned long)cpuHz);
      delay(2000);
    }
  }
  pinMode(PIN_5V_EN, OUTPUT);
  digitalWrite(PIN_5V_EN, PIN_5V_EN_STATE);    // VBUS on the USB-A jack

  pio_usb_configuration_t pioCfg = PIO_USB_DEFAULT_CONFIG;
  pioCfg.pin_dp = PIN_USB_HOST_DP;
  USBHost.configure_pio_usb(1, &pioCfg);
  USBHost.begin(1);
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

void tuh_cdc_umount_cb(uint8_t idx) {
  SerialHost.umount(idx);
  umountEvent = true;
}

}
