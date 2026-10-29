# CLAUDE.md — Arduino (project-specific)

> Working style, machine detection, and GitHub conventions are in the global context:
> `<github-dir>/claude-contexts/CLAUDE.md`
>
> Note: This repo is Mac-only. Arduino deployment requires the Arduino IDE or arduino-cli
> and a USB connection; there is no remote deployment path from the Pi.

## What This Project Does

Two Arduino UNO R4 WiFi sketches that each run a minimal HTTP server serving the current pressure reading from a ratiometric analog pressure sensor. The Raspberry Pi (pivac) polls them over HTTP and feeds the readings into Signal K. See the companion repo at `~/github/pivac` for the consumer side.

## Hardware

All boards are **Arduino UNO R4 WiFi**. The pressure sensor connects to analog pin A0. The board's built-in LED matrix displays the current and max PSI readings.

| Sketch              | Sensor        | IP          | MAC                | Max PSI | Sensor voltage range |
|---------------------|---------------|-------------|--------------------|---------|----------------------|
| ArduinoPSI_BoilerLoop | Fusch 100PSI  | 10.0.0.219  | 34:b7:da:66:1e:50  | 100     | 0.5–4.5V             |
| ArduinoPSI_Domestic   | Fusch 200PSI  | 10.0.0.114  | c0:4e:30:11:6f:3c  | 200     | 0.5–5.0V             |
| DomesticWater         | DAE MJ-75a meter | 10.0.0.188 | 34:b7:da:65:99:1c | —       | reed on D2 (not A0)  |
| AirHandler_MBR        | 2× DS18B20 on D2, 2× 10K NTC on A0/A1, Y2 relay on D8 | 10.0.0.180 | e8:f6:0a:aa:a3:88 | — | PROTO-SHIELD; 24 VAC buck converter (4.65 V at the shield) |

All boards have static IP addresses assigned via DHCP reservation on the router.

> **IP/MAC mapping verified 2026-06-02** against the boards' USB serials and the
> live readings (the `.219`/`34:b7:da:66:1e:50` board reports ~12 PSI — a hydronic
> loop pressure — confirming the 100 PSI BoilerLoop firmware). An earlier version of
> this table had the IP and MAC columns swapped between the two rows; do not re-invert.
> The DS18B20 DHW-recirc sensor is on the **Domestic** board (`.114`) only. NB: the
> pivac-side *module* names are inverted vs role (`pivac.ArduinoPSI` = DHW `.114`,
> `pivac.ArduinoThermPSI` = boiler `.219`) — see pivac `CLAUDE.md`; the sketch names
> here (Domestic = DHW, BoilerLoop = boiler) are correct.

## How It Works

On each loop iteration the sketch checks WiFi connectivity (reconnecting automatically if the connection dropped), reads A0 at 14-bit resolution, converts the ADC count to PSI using the sensor's voltage range, then listens for an incoming HTTP connection. When a request arrives it responds with a single line of pseudo-JSON:

```
{'psi' : 18.400000}
```

Note: single quotes are used, not double quotes — this is intentional. The pivac `ArduinoSensor` module parses the response with `ast.literal_eval` (Python literal syntax), not a JSON parser, so single quotes work fine. Do not change to double quotes without also updating the pivac parser.

The response is wrapped in minimal HTML boilerplate (the original template code), but pivac ignores everything except the `{'psi': ...}` line, which it extracts with a regex.

## Code Structure

All shared logic lives in `ArduinoPSI_BoilerLoop/ArduinoPSI_impl.h`. The copy in `ArduinoPSI_Domestic/` is a symlink pointing to it — there is only one file on disk. Each `.ino` file is a minimal stub that defines only the two sensor-specific constants (`SENSOR_MAX_PSI` and `SENSOR_MAX_V`) and then `#include`s the shared header.

When making logic changes, edit `ArduinoPSI_BoilerLoop/ArduinoPSI_impl.h` directly. The symlink means the Domestic sketch always picks up the same file automatically. The diff between the two `.ino` files should always be just those two constant values.

### Air-handler nodes

`AirHandler/AirHandler_impl.h` holds all logic for the air-handler nodes. Each zone has its own folder, starting with `AirHandler_MBR/`, holding a symlink to that header and a `.ino` that defines only `ZONE_TAG`, `HOSTNAME`, the two DS18B20 ROMs and the NTC constants. The design is in pivac's `docs/mbr-air-handler-node-plan.md` §4, and the shield wiring in `docs/mbr-air-handler-node-shield-review.md`. The node reads water supply and return on D2, supply air on A0, return air on A1 and the Y2 contact on D8. It serves `{'wsup' : …, 'wret' : …, 'aret' : …, 'asup' : …, 'y2' : …, 'y2_s' : …, 'rssi' : …, 'uptime_ms' : …}` and leaves out any sensor that failed to read. The 1-Wire bus is on a DS2482 at I2C 0x18 when one answers at boot, which is the fabricated shield's U2, and on D2 otherwise; `AirHandler/OneWireBus.h` makes the choice, carries the DS2482 driver and the DS18B20 reads, and is symlinked into each sketch folder. The boot log names the master it chose. A zone on the fabricated shield defines `SHIELD_PCB`, which puts return air on A0 and supply on A1 as that board is wired. It prints the bus ROMs and raw counts for ten seconds at boot, before WiFi. While the zone file's ROMs are all zero, it reads the first two probes in search order.

`AirHandlerBench/` is a serial-only sketch for proving a shield's wiring before the node goes on WiFi: ROMs with CRC, both probe temperatures, A0 and A1 in counts, ohms and °F, and D8 with an edge count. Type `r` in the Serial Monitor to re-detect the 1-Wire master and rescan the bus; the boot banner prints before a monitor can open on the R4, so `r` is how the master line is read.

Both build with `arduino-cli compile -b arduino:renesas_uno:unor4wifi <folder>` once the OneWire and ArduinoGraphics libraries are installed. On an Apple Silicon Mac the R4 core's compiler is Intel-only, so it needs Rosetta.

## Known Issues / Notes

**WiFi credentials are in `arduino_secrets.h` (gitignored).** Each sketch folder contains an `arduino_secrets.h` that defines `SECRET_SSID` and `SECRET_PASS`. This file is listed in `.gitignore` and is not committed. If you clone the repo fresh, copy `arduino_secrets.h.example` from the repo root to each sketch folder and fill in your credentials. Note that credentials were previously committed inline in the `.ino` files — the git history retains that, but they are no longer committed going forward.

**Response format is not strict JSON.** Single-quoted keys and floats with trailing zeros are valid Python literals but not valid JSON. Changing to strict JSON would require updating `ArduinoSensor.py` in the pivac repo at the same time.

**`psiMax` resets on reboot only.** The sketch tracks the session maximum PSI and displays it on the LED matrix, but it is a runtime `float` variable and is lost when the board resets.

**LED matrix display.** The matrix alternates every second between current PSI (e.g. ` 18`) and max PSI (e.g. `m42`).

**Self-recovery (watchdog + escalating reconnect).** Both boards run an RA4M1
hardware watchdog (`WDT`, 5 s) armed at the start of `setup()` and refreshed at
the top of every `loop()`, so a fully wedged board self-resets within ~5 s.
`connectWiFi()` is bounded (20 s budget, `WiFi.disconnect()` first, refreshes the
watchdog while waiting); if a reconnect can't complete it calls
`NVIC_SystemReset()` for a clean full reboot, which clears a wedged WiFi module
far more reliably than repeated `WiFi.begin()`. The HTTP request loop is bounded
(`HTTP_CLIENT_TIMEOUT_MS`, 2 s) so a stalled poller can't hang the board. This
targets the observed failure where a board drops off WiFi after an AP/DHCP blip
and never rejoins (requiring a manual power cycle). Tuning constants are in the
`Reliability tuning` block in `ArduinoPSI_impl.h`.

## Chiltrix CX75 Modbus Sketches

Three sketches for the Chiltrix chiller's RS-485 link, all on an UNO R4 plus a DFRobot **DFR0259** shield (both DIP switches AUTO and ON). They are a different shape from the PSI sketches: no shared header, no LED matrix, and `ChiltrixModbus` is the only one that touches WiFi.

| Sketch | Purpose |
|--------|---------|
| `ChiltrixModbus` | The production bridge — polls the chiller and serves one pseudo-JSON line for pivac, same convention as the PSI boards |
| `ChiltrixScan`   | Interactive scanner over USB serial, for confirming addresses and hunting the ones Chiltrix does not document |
| `RS485Blast`     | Diagnostic only — saturates the driver so a DC meter can tell "shield transmitting" from "shield mute" |

**Link settings are fixed by Chiltrix:** Modbus RTU, 9600 8N1, slave ID 1, function 03.

**Wiring is `A3` and `B3` on the chiller, A/B only, with no ground at the Arduino.** The cable drain stays grounded at the chiller end alone. **Fail-safe bias must stay fitted at the Arduino end** — 680 Ω from +5 V to A and 680 Ω from B to GND — because the DFR0259 has no bias network and tri-states its driver between frames, which leaves the pair floating around 10 mV and the receiver free-running on noise. Do not add a terminator; it fights the bias resistors.

**Register addresses come from Chiltrix's own document**, `Cx34-1,2,3/Cx50-2/Cx35-1 Modbus Protocol and Connections Lists`, filed in `~/OneDrive - DGLC/Claude/HVAC Manuals/`. It contradicts the jasipsw CX50-2 community map on every address the two share, so only the documented thirteen are polled. It documents no operating-state or fault-code register; `ChiltrixScan` lists candidates for those but nothing reaches Signal K unconfirmed.

**Both these sketches are read-only — function code 3, never 6 or 16.** Registers 140–146 are writable on the chiller and include the cooling target and the on/off switch. pivac is a monitoring system.

Building `ChiltrixModbus` needs the `ModbusMaster` library (`arduino-cli lib install ModbusMaster`) and an `arduino_secrets.h` in its folder; `ChiltrixScan` and `RS485Blast` need neither WiFi nor credentials.

## Deploying Changes

Arduino sketches are deployed via the Arduino IDE (or Arduino CLI) — there is no OTA update mechanism. To update a board, connect via USB, open the sketch in the Arduino IDE, and upload. Before uploading, ensure `arduino_secrets.h` exists in the sketch folder with the correct credentials (it is gitignored, so a fresh clone will not have it — copy from `arduino_secrets.h.example`). The board's IP and WiFi credentials must match the router and pivac config.

To compile and upload via arduino-cli (the local `libraries/` folder must be passed explicitly since ArduinoGraphics is not in the global library path):

```bash
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi \
  --libraries ArduinoPSI_BoilerLoop/libraries \
  ArduinoPSI_BoilerLoop

arduino-cli upload --fqbn arduino:renesas_uno:unor4wifi \
  --port /dev/cu.usbmodem<id> \
  ArduinoPSI_BoilerLoop
```

The board's port (`/dev/cu.usbmodem...`) changes each time it is plugged in. Use `arduino-cli board list` to find the current port.

## Repository Notes

The remote uses SSH (`git@github.com:dglcinc/Arduino.git`), not HTTPS. The default branch is `main`.

The local clone lives at `~/OneDrive - DGLC/Claude/github/Arduino/`. There is also a stale copy at `~/OneDrive - DGLC/GitHub/Arduino/` — that one is not the active working directory and should not be used.
