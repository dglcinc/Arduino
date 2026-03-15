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

Both units are **Arduino UNO R4 WiFi** boards. The pressure sensor connects to analog pin A0. The board's built-in LED matrix displays the current and max PSI readings.

| Sketch              | Sensor        | IP          | Max PSI | Sensor voltage range |
|---------------------|---------------|-------------|---------|----------------------|
| ArduinoPSI_BoilerLoop | Fusch 100PSI  | 10.0.0.114  | 100     | 0.5–4.5V             |
| ArduinoPSI_Domestic   | Fusch 200PSI  | 10.0.0.219  | 200     | 0.5–5.0V             |

Both boards have static IP addresses assigned via DHCP reservation on the router.

## How It Works

On each loop iteration the sketch reads A0, converts the ADC count to PSI using the sensor's voltage range, then listens for an incoming HTTP connection. When a request arrives it responds with a single line of pseudo-JSON:

```
{'psi' : 18.400000}
```

Note: single quotes are used, not double quotes — this is intentional. The pivac `ArduinoSensor` module parses the response with `ast.literal_eval` (Python literal syntax), not a JSON parser, so single quotes work fine. Do not change to double quotes without also updating the pivac parser.

The response is wrapped in minimal HTML boilerplate (the original template code), but pivac ignores everything except the `{'psi': ...}` line, which it extracts with a regex.

## Key Differences Between the Two Sketches

The sketches are nearly identical. The only meaningful differences are `sensorMaxPsi` (100 vs 200) and `sensorMaxV` (4.5V vs 5.0V), which reflect the two different sensor models. The Domestic sketch also has slightly more debug `Serial.println` calls.

## Known Issues / Notes

**WiFi credentials are hardcoded in the .ino files.** The sketches include a comment directing credentials to `arduino_secrets.h`, but the actual `ssid` and `pass` variables are set inline in the code, and `arduino_secrets.h` is blank. The credentials are therefore committed to the repo. This is a known issue — if credentials need to change, update them in the `.ino` file directly (and be mindful of git history).

**Response format is not strict JSON.** Single-quoted keys and floats with trailing zeros are valid Python literals but not valid JSON. Changing to strict JSON would require updating `ArduinoSensor.py` in the pivac repo at the same time.

**`psiMax` resets on reboot only.** The sketch tracks the session maximum PSI and displays it on the LED matrix, but it's a runtime variable and is lost when the board resets.

**LED matrix display.** The matrix alternates every second between current PSI (e.g. ` 18`) and max PSI (e.g. `m42`).

## Deploying Changes

Arduino sketches are deployed via the Arduino IDE (or Arduino CLI) — there is no OTA update mechanism. To update a board, connect via USB, open the sketch in the Arduino IDE, and upload. The board's IP and WiFi credentials must match the router and pivac config.

## Repository Notes

The remote uses SSH (`git@github.com:dglcinc/Arduino.git`), not HTTPS. The default branch is `main`.
