// DomesticWater — whole-house water meter node (UNO R4 WiFi)
//
// Meter: DAE MJ-75a, 0.1 gal/pulse, 2-wire dry-contact reed on D2.
//
// METER-ONLY build (valve deferred 2026-07-03): the motorized-shutoff valve was
// dropped from scope. The DPDT-relay/valve version of this sketch is in git
// history on this branch if the valve is ever added back. With no valve there is
// no 12 V rail — power the board over USB.
//
// Reuses the proven scaffolding pattern from the ArduinoPSI_* sketches:
//   - WiFiS3 HTTP server on port 80, single-line single-quoted status dict
//     (pivac.ArduinoSensor parses with ast.literal_eval, NOT JSON — keep single quotes)
//   - RA4M1 hardware watchdog (WDT) armed in setup(), refreshed every loop()
//   - bounded connectWiFi() with NVIC_SystemReset() fallback for a wedged module
//   - bounded HTTP client loop so a stalled poller can't hang the board
//
// pivac stays READ-ONLY: it only GETs /.
// See pivac docs/domestic-water-node-build-spec.md.
//
// WiFi creds come from arduino_secrets.h (gitignored — copy from the example).

#include "ArduinoGraphics.h"      // must precede Arduino_LED_Matrix
#include "Arduino_LED_Matrix.h"
#include "WiFiS3.h"
#include "WDT.h"                  // RA4M1 hardware watchdog
#include "EEPROM.h"
#include "arduino_secrets.h"      // SECRET_SSID / SECRET_PASS

// ---------------------------------------------------------------------------
// Meter configuration
// ---------------------------------------------------------------------------
const float    K_GAL_PER_PULSE = 0.1f;     // DAE MJ-75a factory K-factor (±1.5%)
const uint8_t  PIN_PULSE       = 2;        // reed, INPUT_PULLUP, FALLING edge

const unsigned long DEBOUNCE_US    = 3000;        // reed bounce guard (~max 2 Hz)
const unsigned long FLOW_WINDOW_MS = 10000UL;     // rolling flow-rate window
const unsigned long EEPROM_SAVE_MS = 300000UL;    // persist totalizer every 5 min

// EEPROM layout (R4 renesas core writes through; no commit() needed)
// (addr 8 held the valve state in the valve-era layout; left unused)
const int      EE_TOTAL_ADDR = 0;          // uint32_t lifetime pulse count
const int      EE_MAGIC_ADDR = 12;         // uint32_t validity marker
const uint32_t EE_MAGIC      = 0xDA00075AUL; // "DAE MJ-75a" — distinguishes fresh flash

// Reliability tuning (mirrors ArduinoPSI_impl.h) ---------------------------
const uint32_t      WDT_TIMEOUT_MS         = 5000;
const unsigned long WIFI_CONNECT_BUDGET_MS = 20000;
const unsigned long HTTP_CLIENT_TIMEOUT_MS = 2000;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
volatile uint32_t      pulseCount  = 0;    // pulses since last loop drain (ISR)
volatile unsigned long lastPulseUs = 0;    // debounce timestamp (ISR)

uint32_t totalPulses     = 0;              // persisted lifetime total
uint32_t lastSavedTotal  = 0;
unsigned long lastSaveMs = 0;

uint32_t      windowStartPulses = 0;       // rolling flow window
unsigned long windowStartMs     = 0;
float         flowRateGpm       = 0.0f;

char ssid[] = SECRET_SSID;
char pass[] = SECRET_PASS;

char jsonResponse[120];
char output[120];
char displayText[16];

WiFiServer       server(80);
ArduinoLEDMatrix matrix;

unsigned long lastDisplayChange = 0;

// ---------------------------------------------------------------------------
// ISR — one reed closure = K_GAL_PER_PULSE gallons. Software-debounced.
// ---------------------------------------------------------------------------
void onPulse() {
  unsigned long now = micros();
  if (now - lastPulseUs < DEBOUNCE_US) return;
  lastPulseUs = now;
  pulseCount++;
}

void saveTotal() {
  EEPROM.put(EE_TOTAL_ADDR, totalPulses);
  lastSavedTotal = totalPulses;
  lastSaveMs = millis();
}

// ---------------------------------------------------------------------------
// WiFi (bounded, watchdog-aware) — identical strategy to ArduinoPSI_impl.h
// ---------------------------------------------------------------------------
bool connectWiFi(unsigned long budgetMs) {
  unsigned long start = millis();
  Serial.print("Connecting to SSID: ");
  Serial.println(ssid);
  WiFi.disconnect();
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > budgetMs) return false;
    WDT.refresh();
    delay(250);
  }
  return true;
}

void printWifiStatus() {
  Serial.print("SSID: ");        Serial.println(WiFi.SSID());
  Serial.print("IP Address: ");  Serial.println(WiFi.localIP());
  Serial.print("RSSI: ");        Serial.print(WiFi.RSSI()); Serial.println(" dBm");
}

// ---------------------------------------------------------------------------
// HTTP response helpers
// ---------------------------------------------------------------------------
void sendHeaders(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html");
  client.println("Connection: close");
  client.println();
}

// Status dict — single quotes are intentional (pivac uses ast.literal_eval).
// volume = totalPulses * K; flowing derived; uptime_ms lets pivac tell a
// self-reconnect (climbing) from a reboot (resets to ~0).
void sendStatus(WiFiClient &client) {
  float volume = totalPulses * K_GAL_PER_PULSE;
  int   flowing = (flowRateGpm > 0.0f) ? 1 : 0;
  sendHeaders(client);
  client.println("<!DOCTYPE HTML>");
  client.println("<html>");
  sprintf(jsonResponse,
          "{'flow' : %.2f, 'volume' : %.1f, 'flowing' : %d, 'uptime_ms' : %lu}",
          flowRateGpm, volume, flowing, millis());
  client.println(jsonResponse);
  client.println("</html>");
}

void sendOk(WiFiClient &client, const char *msg) {
  sendHeaders(client);
  client.println("<!DOCTYPE HTML><html>");
  client.println(msg);
  client.println("</html>");
}

// Route on the captured request line (e.g. "GET /reset?confirm=1 HTTP/1.1").
void handleRequest(WiFiClient &client, const char *reqLine) {
  if (strstr(reqLine, "GET /reset?confirm=1")) {
    totalPulses = 0; saveTotal();
    windowStartPulses = 0;
    Serial.println("Totalizer RESET");
    sendOk(client, "{'reset' : 1}");
  } else {
    sendStatus(client);   // GET / and everything else
  }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  if (!WDT.begin(WDT_TIMEOUT_MS)) {
    Serial.println("WDT.begin failed (timeout out of range?)");
  } else {
    Serial.println("Watchdog armed.");
  }

  // Restore the persisted totalizer. On a truly fresh board the magic is
  // absent → start at 0 gallons and stamp the magic.
  uint32_t magic = 0;
  EEPROM.get(EE_MAGIC_ADDR, magic);
  if (magic == EE_MAGIC) {
    EEPROM.get(EE_TOTAL_ADDR, totalPulses);
  } else {
    totalPulses = 0;
    EEPROM.put(EE_TOTAL_ADDR, totalPulses);
    EEPROM.put(EE_MAGIC_ADDR, EE_MAGIC);
  }
  lastSavedTotal = totalPulses;

  pinMode(PIN_PULSE, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_PULSE), onPulse, FALLING);

  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    while (true);
  }
  String fv = WiFi.firmwareVersion();
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) Serial.println("Please upgrade the WiFi firmware");

  while (!connectWiFi(WIFI_CONNECT_BUDGET_MS)) {
    Serial.println("Initial WiFi connect timed out; retrying...");
  }
  server.begin();
  printWifiStatus();
  matrix.begin();

  windowStartMs     = millis();
  windowStartPulses = totalPulses;
  lastSaveMs        = millis();
}

void loop() {
  WDT.refresh();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost — reconnecting...");
    if (!connectWiFi(WIFI_CONNECT_BUDGET_MS)) {
      Serial.println("Reconnect failed; resetting board.");
      delay(50);
      NVIC_SystemReset();
    }
    printWifiStatus();
  }

  // Fold ISR pulses into the lifetime total.
  noInterrupts();
  uint32_t pc = pulseCount; pulseCount = 0;
  interrupts();
  totalPulses += pc;

  // Rolling flow rate over FLOW_WINDOW_MS.
  unsigned long nowMs = millis();
  if (nowMs - windowStartMs >= FLOW_WINDOW_MS) {
    uint32_t dp = totalPulses - windowStartPulses;
    float minutes = (nowMs - windowStartMs) / 60000.0f;
    flowRateGpm = (minutes > 0.0f) ? (dp * K_GAL_PER_PULSE) / minutes : 0.0f;
    windowStartPulses = totalPulses;
    windowStartMs = nowMs;
  }

  // Persist totalizer on a timer (never per-pulse — EEPROM wear).
  if (totalPulses != lastSavedTotal && nowMs - lastSaveMs >= EEPROM_SAVE_MS) saveTotal();

  // Serve one pending HTTP request (bounded).
  WiFiClient client = server.available();
  if (client) {
    unsigned long clientStart = millis();
    char reqLine[128];
    int  idx = 0;
    bool haveReqLine = false;
    bool currentLineIsBlank = true;
    while (client.connected()) {
      if (millis() - clientStart > HTTP_CLIENT_TIMEOUT_MS) break;
      if (client.available()) {
        char c = client.read();
        if (!haveReqLine) {
          if (c == '\n') { reqLine[idx] = '\0'; haveReqLine = true; }
          else if (c != '\r' && idx < (int)sizeof(reqLine) - 1) reqLine[idx++] = c;
        }
        if (c == '\n' && currentLineIsBlank) {
          if (!haveReqLine) reqLine[idx] = '\0';
          handleRequest(client, reqLine);
          break;
        }
        if (c == '\n') currentLineIsBlank = true;
        else if (c != '\r') currentLineIsBlank = false;
      }
    }
    delay(1);
    client.stop();
  }

  // LED matrix: current flow, whole gpm.
  if (nowMs - lastDisplayChange >= 1000) {
    lastDisplayChange = nowMs;
    sprintf(displayText, "%d", (int)(flowRateGpm + 0.5f));
    matrix.beginDraw();
    matrix.stroke(0xFFFFFFFF);
    matrix.textFont(Font_4x6);
    matrix.beginText(1, 2, 0xFFFFFF);
    matrix.println(displayText);
    matrix.endText();
    matrix.endDraw();
  }
}
