#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright David Lewis
"""MqttLite and MqttDiscovery against a real broker: needs `pip install amqtt paho-mqtt`. Run from AirHandlerPico/test. Proves the
broker accepts the packets, every discovery payload is valid JSON with the keys Home Assistant requires, the will fires on a
dropped link, commands arrive with their retain flag, and a refused login is reported. It does not run Home Assistant."""
import asyncio, json, os, subprocess, sys, tempfile, threading, time
import paho.mqtt.client as paho
from amqtt.broker import Broker
PORT = 18883
exe = os.path.join(tempfile.gettempdir(), "am_mqtt_host")
subprocess.run(["c++", "-std=c++17", "-Wall", "-I..", "mqtt_host.cpp", "-o", exe], check=True)
cfg = {"listeners": {"default": {"type": "tcp", "bind": "127.0.0.1:%d" % PORT, "max_connections": 20}},
       "auth": {"allow-anonymous": True, "plugins": ["auth_anonymous"]}, "topic-check": {"enabled": False}, "sys_interval": 0}
loop = asyncio.new_event_loop()
def run():
    asyncio.set_event_loop(loop)
    broker = Broker(cfg, loop=loop)
    loop.run_until_complete(broker.start())
    loop.run_forever()
threading.Thread(target=run, daemon=True).start()
time.sleep(1.5)

seen, retained = {}, {}
def on_message(c, u, m):
    p = m.payload.decode()
    seen.setdefault(m.topic, []).append(p)
    if m.retain:
        retained[m.topic] = p
sub = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id="check")
sub.on_message = on_message
sub.connect("127.0.0.1", PORT)
sub.subscribe([("homeassistant/#", 0), ("airmaster/#", 0)])
sub.loop_start()
time.sleep(0.5)
problems = []
def ok(c, what):
    print(("ok   " if c else "FAIL ") + what)
    if not c:
        problems.append(what)

p = subprocess.Popen([exe, str(PORT), "", "", "writes"], stdout=subprocess.PIPE, text=True)
lines = []
threading.Thread(target=lambda: [lines.append(l.strip()) for l in p.stdout], daemon=True).start()
def wait_line(s, t=8):
    end = time.time() + t
    while time.time() < end:
        if any(s in l for l in lines):
            return True
        time.sleep(0.05)
    return False
ok(wait_line("ready 1"), "the broker accepts the CONNECT")
ok(wait_line("published"), "every message goes out")
time.sleep(1)
cfgs = {t: v for t, v in seen.items() if t.startswith("homeassistant/") and t.endswith("/config")}
ok(len(cfgs) == 19, "19 discovery messages (%d)" % len(cfgs))
bad = []
for t, v in cfgs.items():
    d = json.loads(v[-1])
    need = {"name", "unique_id", "state_topic", "availability_topic", "device", "value_template"}
    if "/number/" in t:
        need |= {"command_topic", "min", "max", "step"}
    if not need <= set(d) or d["device"]["identifiers"] != ["airmaster_4f2a"]:
        bad.append(t)
ok(not bad, "each is valid JSON with the required keys, even with quotes in the unit's name")
ok(len({json.loads(v[-1])["unique_id"] for v in cfgs.values()}) == len(cfgs), "unique ids are unique")
ok(json.loads(cfgs["homeassistant/sensor/airmaster_4f2a/wsup/config"][-1])["device"]["name"] == 'Kid\'s "Room"', "the name survives escaping")
ok(json.loads(seen["airmaster/4f2a/state"][-1])["wsup"] == 49.81, "the state message arrives")
ok(seen["airmaster/4f2a/status"][-1] == "online", "online published")

sub.publish("airmaster/4f2a/set/cfm/Y2", "525")
ok(wait_line("rx airmaster/4f2a/set/cfm/Y2|525|0|cfm.Y2"), "a command arrives and maps to the job's key")
sub.publish("airmaster/4f2a/set/rpm/Y2", "900")
ok(wait_line("set/rpm/Y2|900|0|-"), "only stored airflow maps; an RPM limit is refused")
sub.publish("airmaster/4f2a/set/cfm/X", "525")
ok(wait_line("set/cfm/X|525|0|-"), "an unknown call is refused")
sub.publish("airmaster/4f2a/set/die", "die")
ok(wait_line("dropped"), "the link drops without a goodbye")
time.sleep(1.5)
ok(seen["airmaster/4f2a/status"][-1] == "offline", "the broker publishes the will: offline")

# A retained command is flagged, so the firmware can ignore it on every reconnect
sub.publish("airmaster/4f2a/set/cfm/G", "999", retain=True)
time.sleep(0.5)
lines.clear()
p = subprocess.Popen([exe, str(PORT), "", "", "off"], stdout=subprocess.PIPE, text=True)
threading.Thread(target=lambda: [lines.append(l.strip()) for l in p.stdout], daemon=True).start()
ok(wait_line("set/cfm/G|999|1|cfm.G"), "a retained command arrives with its retain flag set")
sub.publish("airmaster/4f2a/set/die", "die"); wait_line("dropped")
time.sleep(0.5)
ok(seen["homeassistant/number/airmaster_4f2a/cfm_y2/config"][-1] == "", "with changes off, the airflow numbers are removed")
late = paho.Client(paho.CallbackAPIVersion.VERSION2, client_id="late")     # only a later subscriber sees the retain flag
late.on_message = on_message
late.connect("127.0.0.1", PORT)
late.subscribe("homeassistant/#")
late.loop_start()
time.sleep(1)
ok("homeassistant/sensor/airmaster_4f2a/wsup/config" in retained, "a client that connects later still finds the discovery messages")
print("%d problems" % len(problems), flush=True)
os._exit(1 if problems else 0)
