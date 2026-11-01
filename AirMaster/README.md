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

`list` shows every unit without its password.

The ledger, `~/.config/airmaster/units.csv`, and the PDFs hold every password. They stay outside this repo and readable only by you. Back up the ledger.

## First setup, as the owner sees it

1. Power the unit with the USB cable to the Unico plugged in.
2. Scan the label's QR code, or join `AirMaster-XXXX` with the label's password. A setup page opens, or browse to `http://192.168.42.1`.
3. Choose the home WiFi, name the unit and set an admin password.
4. The unit restarts onto the home WiFi. Open `http://<name>.local`.

If the join fails, the setup network returns within about a minute and its page gives the reason. If the unit loses its WiFi later, the setup network comes up until someone uses it or 10 minutes pass with nobody on it, then the unit tries again. For a forgotten admin password, power it off and on three times, within 30 seconds each, and join the setup network.

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

After editing `web/index.html`, run `AirMaster/web/build_page.py`, which writes `AirHandlerPico/web_page.h`.
