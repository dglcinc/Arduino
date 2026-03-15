# CLAUDE.md

This file provides guidance to Claude when working with code in this repository.

## Working Style

- **Execute without repeated check-ins.** Before a multi-step task, state the plan briefly and confirm once. Then carry out all steps without asking permission at each one.
- **Targeted edits, not rewrites.** When modifying an existing file, make surgical changes to the relevant lines. Do not rewrite or reorder content that isn't changing — it creates noise in diffs and risks dropping things accidentally.
- **PR workflow for code.** Always create a feature branch and open a pull request for code changes. Only push directly to `main` for meta/context files (CLAUDE.md).
- **Keep CLAUDE.md current.** After significant changes — new hardware, sensor changes, bug fixes — update this file and include it in the commit.
- **No unnecessary confirmation loops.** Don't ask "should I proceed?" or "does this look right?" mid-task. Finish the work, then summarize what was done.
- **Commit message quality.** Write commit messages that explain why, not just what. Reference the problem being solved, not just the files changed.
- **Prose over bullets in explanations.** When explaining an approach or decision, write in sentences rather than fragmenting everything into bullet lists.

## What This Project Does

Two Arduino UNO R4 WiFi sketches that each run a minimal HTTP server serving the current pressure reading from a ratiometric analog pressure sensor. The Raspberry Pi (pivac) polls them over HTTP and feeds the readings into Signal K. See the companion repo at `~/github/pivac` for the consumer side.

## Hardware

All boards are **Arduino UNO R4 WiFi**. The pressure sensor connects to analog pin A0. The board's built-in LED matrix displays the current and max PSI readings.

| Sketch              | Sensor        | IP          | MAC                | Max PSI | Sensor voltage range |
|---------------------|---------------|-------------|--------------------|---------|----------------------|
| ArduinoPSI_BoilerLoop | Fusch 100PSI  | 10.0.0.114  | c0:4e:30:11:6f:3c  | 100     | 0.5–4.5V             |
| ArduinoPSI_Domestic   | Fusch 200PSI  | 10.0.0.219  | 34:b7:da:66:1e:50  | 200     | 0.5–5.0V             |
| (experimental)        | —             | 10.0.0.188  | 34:b7:da:65:99:1c  | —       | —                    |

All boards have static IP addresses assigned via DHCP reservation on the router.

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

## Known Issues / Notes

**WiFi credentials are in `arduino_secrets.h` (gitignored).** Each sketch folder contains an `arduino_secrets.h` that defines `SECRET_SSID` and `SECRET_PASS`. This file is listed in `.gitignore` and is not committed. If you clone the repo fresh, copy `arduino_secrets.h.example` from the repo root to each sketch folder and fill in your credentials. Note that credentials were previously committed inline in the `.ino` files — the git history retains that, but they are no longer committed going forward.

**Response format is not strict JSON.** Single-quoted keys and floats with trailing zeros are valid Python literals but not valid JSON. Changing to strict JSON would require updating `ArduinoSensor.py` in the pivac repo at the same time.

**`psiMax` resets on reboot only.** The sketch tracks the session maximum PSI and displays it on the LED matrix, but it is a runtime `float` variable and is lost when the board resets.

**LED matrix display.** The matrix alternates every second between current PSI (e.g. ` 18`) and max PSI (e.g. `m42`).

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
