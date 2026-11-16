<!-- SPDX-License-Identifier: CC-BY-NC-4.0 -->
<!-- Copyright David Lewis -->

# AirMaster: build, label and flash

An AirMaster works with no pivac. It serves its own web page, and its owner connects with a phone, tablet or computer. Each unit has its own setup password, printed on its label.

## Make a page of labels

```bash
python3 -m venv ~/.config/airmaster/venv
~/.config/airmaster/venv/bin/pip install -r AirMaster/factory/requirements.txt
~/.config/airmaster/venv/bin/python AirMaster/factory/airmaster_factory.py sheet --print
```

`sheet` makes 12 new identities, records them in the ledger and prints a Letter page of 12 labels, in serial order. Print at 100 % and check the 50 mm bar. The PDF is kept in `~/.config/airmaster/labels`.

## Flash boards

```bash
arduino-cli compile -b rp2040:rp2040:rpipicow:usbstack=tinyusb_host,flash=2097152_1048576 --output-dir build AirMaster
~/.config/airmaster/venv/bin/python AirMaster/factory/airmaster_factory.py flash --uf2 build/AirMaster.ino.uf2 --count 5
```

Build against Adafruit TinyUSB Library 3.7.4 (`arduino-cli lib install "Adafruit TinyUSB Library@3.7.4"`). A build with 3.7.7 mounts the Unico USB board and reads bytes from it, but no SCB frame ever parses.

1. Unplug 24 VAC and the USB-A cable. The micro-USB port and the converter share pin 40, so a computer must never meet 24 VAC.
2. Hold BOOTSEL, plug USB into the micro-USB port and let go.
3. The tool loads the firmware with the next unused identity, reads it back and names the label, such as `Use label AM-0005`.
4. Stick that label on that box.

| Case | What happens |
|---|---|
| Flash fails | The label stays unused for the next board |
| Board already has an identity | It keeps it, and the tool names its existing label |
| No unused labels | The tool stops and asks for a new sheet |
| Label damaged | `labels AM-0005` prints it again |

`list` shows every unit without its password. A setup password is eight lowercase letters and digits, without 0, 1, i, l or o. `retire` voids every unused label in the ledger, so the next flash takes one from a newer sheet; destroy the voided labels. `retire --all` voids flashed units too; re-flash each with `flash --new-identity`, which keeps the unit's saved settings, and stick on the label it names. `flash --serial AM-0014` gives a replacement board that unit's identity, so the serial and label move to it; then re-flash or retire the old board, since two boards must never share an identity.

The ledger, `~/.config/airmaster/units.csv`, and the PDFs hold every password. They stay outside this repo and readable only by you. Back up the ledger.

## First setup, as the owner sees it

1. Power the unit with the USB cable to the Unico plugged in.
2. Scan the label's QR code, or join `AirMaster-XXXX` with the label's password. A setup page opens, or browse to `http://192.168.42.1`.
3. Choose the home WiFi and name the unit. The password on the label is also the password for changing settings; the Unit tab changes it.
4. The unit restarts onto the home WiFi. Open `http://<name>.local`.

If the join fails, the setup network returns within about a minute and its page gives the reason. If the unit loses its WiFi later, the setup network comes up until someone uses it or 10 minutes pass with nobody on it, then the unit tries again. For a forgotten password, power it off and on three times, within 30 seconds each, join the setup network and save: the password goes back to the one on the label.


With more than one AirMaster on the network, each one's page shows a tile for every unit under its name, with the unit's type, the call in force and the blower's airflow and speed, and a tap on a tile opens that unit. The tiles run across the top of a wide screen and stack on a phone. Each page reads the other units' `/data` lines for them, which every unit allows from another origin. Also,

**Behind a reverse proxy.** The page asks only for relative addresses, so a proxy can serve each unit under a path named for its host, such as `/airmaster/master-bedroom/` and `/airmaster/great-room/`, with the trailing slash. Served that way the tiles link to the sibling paths, so the units must share one parent path and each path must end in the unit's host name. The proxy should send the unit only its own `s` cookie, rewrite that cookie's path to the unit's path, keep every request line under 400 characters, and put its own login in front, because the Settings tab changes the air handler. An nginx example is in pivac's `CLAUDE.md` under Remote Access.

`http://airmaster.local/` always reaches one of them: the unit with the earliest serial takes that name, and if it goes off the network the next one takes it within a few minutes. Each unit also keeps its own name, such as `http://master-bedroom.local/`, while it does not hold `airmaster.local`.
## Home Assistant

On the Unit page, under Home Assistant (MQTT), give the broker's address, and a user name and password if it needs them. Each unit then appears in Home Assistant as a device, with its four temperatures, airflow, blower speed, WiFi signal and six thermostat calls.

Changing stored airflow from Home Assistant has its own switch, off by default. With it on, six number entities appear. They obey the same limits as the page and refuse retained commands. They allow one change per 10 minutes and 24 per power-up, since each one writes to the SCB's memory. The reason for each refusal is published on `airmaster/<id>/write_result`.

The connection has no TLS. Topics are `airmaster/<id>/state`, `/status`, `/write_result` and `/set/cfm/<G|Y1|Y2|W1|W2|E>`, with discovery under `homeassistant/`.

## Tests

| Covers | Command |
|---|---|
| Identity, config, passwords, HTTP | `cd AirHandlerPico/test && python3 check_unit.py` |
| MQTT against a real broker, in a Python 3.11 venv with `amqtt paho-mqtt` | `cd AirHandlerPico/test && python -u check_mqtt.py` |
| Factory flow, simulated board | `python3 AirMaster/factory/test_factory.py build/AirMaster.ino.uf2` |
| The page in headless Chrome, against `web/sim.py` | `python3 AirMaster/web/test_page.py` (needs `playwright`) |

After editing `web/index.html`, run `AirMaster/web/build_page.py`, which writes `AirHandlerPico/web_page.h`. The same header carries the home-screen icons and their web manifest: a phone that adds the page to its home screen shows the DL monogram, from `/apple-touch-icon.png` on iOS and from `/manifest.webmanifest` on Android. `web/gen_icon.py` (Pillow) draws `icon.svg` and the three PNGs; run it only when the icon changes, then run `build_page.py`.
