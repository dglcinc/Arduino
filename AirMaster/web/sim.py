#!/usr/bin/env python3
"""A stand-in for the unit, to develop and test the page in a browser with no hardware: python3 sim.py [setup|fallback|recovery|normal]
Serves index.html and answers the API from memory, with the firmware's rules for login, the X-AirMaster header and the backup.
The admin password starts as testpass1 in any mode but setup. It runs on localhost only."""
import http.server, json, os, secrets, sys, time, urllib.parse
HERE = os.path.dirname(os.path.abspath(__file__))
MODE = sys.argv[1] if len(sys.argv) > 1 else "normal"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8765
M = ("G", "Y1", "Y2", "W1", "W2", "E")
S = {"cfm": dict(zip(M, (300, 300, 600, 300, 600, 600))), "rpm": dict(zip(M, (900, 1200, 1800, 1200, 1800, 1800))),
     "air": {"percent": 0, "blower": 300, "sqft": 1500, "occupants": 4, "runmin": 20, "interval": 4, "bedrooms": 3, "eac": "off", "mode": "fixed"},
     "auto": {"style": "maxcomfort"}, "damper": "powerclose", "cfm_max": 750, "rpm_min": 400, "rpm_max": 1800, "age_s": 0}
st = {"mode": MODE, "name": "" if MODE == "setup" else "Master Bedroom", "admin": "" if MODE == "setup" else "testpass1", "sessions": set(), "backup": None,
      "job_done": 0.0, "result": "none", "error": "", "t0": time.time(), "probes": [{"rom": "28aabbccdd000001", "role": "sup", "f": 49.81}, {"rom": "28aabbccdd000002", "role": "ret", "f": 55.37}],
      "off": {"wsup": 0, "wret": 0, "asup": 0, "aret": 0}, "by_order": 1, "log": [], "mqtt": {"host": "", "port": 1883, "user": "", "pass": "", "writes": 0}}
def snapshot():
    return json.loads(json.dumps(S))
def job(result):
    st["job_done"] = time.time() + 1.5
    st["result"] = result
    if st["backup"] is None:
        st["backup"] = snapshot()
def apply(k, v):
    a, _, b = k.partition(".")
    if a in ("cfm", "rpm"):
        if b not in M: return False
        n = int(v)
        if a == "cfm" and not 0 < n <= S["cfm_max"]: raise ValueError("airflow outside this model's range")
        if a == "rpm" and not S["rpm_min"] <= n <= S["rpm_max"]: raise ValueError("RPM limit outside this model's range")
        S[a][b] = n
    elif a == "air" and b in S["air"]:
        S["air"][b] = v if b in ("eac", "mode") else int(v)
    elif k == "auto.style": S["auto"]["style"] = v
    elif k == "damper": S["damper"] = v
    else: return False
    return True
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, obj, cookie=None):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b)))
        if cookie: self.send_header("Set-Cookie", cookie)
        self.end_headers(); self.wfile.write(b)
    def logged(self):
        c = self.headers.get("Cookie", "")
        return any(p.strip()[2:] in st["sessions"] for p in c.split(";") if p.strip().startswith("s="))
    def do_GET(self):
        p = urllib.parse.urlparse(self.path).path
        if p == "/" and "text/html" in self.headers.get("Accept", ""):
            b = open(os.path.join(HERE, "index.html"), "rb").read()
            self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b); return
        if p in ("/", "/data"):
            t = time.time() - st["t0"]
            d = {"wsup": 49.81 + st["off"]["wsup"], "wret": 55.37 + st["off"]["wret"], "aret": 76.12 + st["off"]["aret"], "asup": 58.4 + st["off"]["asup"], "g": 1, "y1": 1, "y2": 0, "w1": 0, "w2": 0, "e": 0,
                 "cfm": S["cfm"]["Y1"], "rpm": 1045, "flags": 32, "scb_age": .4, "y2_s": 4312, "cal": 1 if any(st["off"].values()) else 0, "rssi": -61, "uptime_ms": int(t * 1000) + 5000000, "reboots": 0}
            return self.send(200, d)
        if p == "/settings":
            busy = time.time() < st["job_done"]
            o = {"job": "working" if busy else "idle", "result": "working" if busy else st["result"], "backup": 1 if st["backup"] else 0}
            if st["backup"] is not None: o.update(snapshot())
            return self.send(200, o)
        if p == "/api/info":
            setup = st["mode"] != "normal"
            nets = [{"ssid": "redux-test", "rssi": -48, "open": 0}, {"ssid": "Neighbor's \"Net\" <b>x</b>", "rssi": -80, "open": 0}, {"ssid": "CoffeeOpen", "rssi": -70, "open": 1}] if setup else []
            return self.send(200, {"mode": st["mode"], "fw": "1.0.0", "serial": "AM-0001", "name": st["name"], "host": hostname(st["name"]), "ap": "AirMaster-4F2A", "ssid": "" if setup else "redux-test",
                                   "ip": "" if setup else "10.0.0.50", "error": st["error"], "admin": 1 if st["admin"] else 0, "wifi": 0 if st["mode"] == "setup" else 1, "logged_in": 1 if self.logged() else 0,
                                   "cal": 1 if any(st["off"].values()) else 0, "by_order": st["by_order"], "backup": 1 if st["backup"] else 0, "mqtt_host": st["mqtt"]["host"], "mqtt_port": st["mqtt"]["port"], "mqtt_user": st["mqtt"]["user"], "mqtt_pass_set": 1 if st["mqtt"]["pass"] else 0, "mqtt_writes": st["mqtt"]["writes"], "mqtt_state": "connected" if st["mqtt"]["host"] else "off", "offsets": st["off"], "probes": st["probes"], "networks": nets})
        return self.send(404, {"error": "not found"})
    def do_POST(self):
        p = urllib.parse.urlparse(self.path).path
        n = int(self.headers.get("Content-Length", 0)); raw = self.rfile.read(n)
        if not self.headers.get("X-AirMaster"): return self.send(403, {"error": "use the AirMaster page"})
        if p == "/api/update":
            if not self.logged(): return self.send(401, {"error": "Log in first."})
            st["log"].append("update %d" % n); return self.send(200, {"ok": 1})
        f = {k: v[0] for k, v in urllib.parse.parse_qs(raw.decode(), keep_blank_values=True).items()}
        if p == "/api/setup":
            if st["mode"] == "normal": return self.send(403, {"error": "setup is only open on the unit's own network"})
            if not f.get("ssid") and st["mode"] == "setup": return self.send(400, {"error": "Choose a WiFi network."})
            if not f.get("admin") and not st["admin"]: return self.send(400, {"error": "Set an admin password."})
            if f.get("admin") and len(f["admin"]) < 8: return self.send(400, {"error": "The admin password needs at least 8 characters."})
            if f.get("name"): st["name"] = f["name"]
            if f.get("admin"): st["admin"] = f["admin"]
            st["log"].append("setup " + json.dumps({k: v for k, v in f.items() if "pass" not in k and k != "admin"}))
            return self.send(200, {"host": hostname(st["name"]), "ap": "AirMaster-4F2A", "ok": 1})
        if p == "/api/login":
            if f.get("password") != st["admin"]: return self.send(401, {"error": "Wrong password."})
            t = secrets.token_hex(16); st["sessions"].add(t)
            return self.send(200, {"ok": 1}, "s=%s; Path=/; Max-Age=43200; HttpOnly; SameSite=Strict" % t)
        if not self.logged(): return self.send(401, {"error": "Log in first."})
        if p == "/api/logout": st["sessions"].clear(); return self.send(200, {"ok": 1})
        if p == "/api/settings/read": job("read"); return self.send(202, {"job": "started"})
        if p == "/api/set":
            if st["backup"] is None: return self.send(409, {"error": "read_first"})
            try:
                for k, v in f.items():
                    if not apply(k, v): return self.send(400, {"error": "bad setting " + k})
            except ValueError as e:
                job(str(e)); return self.send(202, {"job": "started"})
            job("stored"); st["log"].append("set " + json.dumps(f)); return self.send(202, {"job": "started"})
        if p == "/api/restore":
            for k in ("cfm", "rpm", "air", "auto", "damper"): S[k] = json.loads(json.dumps(st["backup"][k]))
            job("stored"); return self.send(202, {"job": "started"})
        if p == "/api/unit":
            if f.get("name"): st["name"] = f["name"]
            if "mqtthost" in f:
                port = int(f.get("mqttport") or 0)
                if not 1 <= port <= 65535: return self.send(400, {"error": "The MQTT port must be 1 to 65535."})
                st["mqtt"].update(host=f["mqtthost"], port=port, user=f.get("mqttuser", ""), writes=1 if f.get("mqttwrites") == "1" else 0)
                if f.get("mqttpass"): st["mqtt"]["pass"] = f["mqttpass"]
            if f.get("admin"):
                if len(f["admin"]) < 8: return self.send(400, {"error": "The admin password needs at least 8 characters."})
                st["admin"] = f["admin"]
            for k in ("wsup", "wret", "asup", "aret"):
                if "off" + k in f: st["off"][k] = float(f["off" + k])
            if "romsup" in f:
                if f["romsup"] == f["romret"]: return self.send(400, {"error": "Choose two different probes."})
                st["by_order"] = 0
                for pr in st["probes"]: pr["role"] = "sup" if pr["rom"] == f["romsup"] else "ret" if pr["rom"] == f["romret"] else ""
            return self.send(200, {"ok": 1})
        if p in ("/api/reboot", "/api/factory-reset"): return self.send(200, {"ok": 1})
        return self.send(404, {"error": "not found"})
def hostname(n):
    import re
    return re.sub(r"[^a-z0-9]+", "-", n.lower()).strip("-") or "airmaster-4f2a"
print("http://localhost:%d mode %s" % (PORT, MODE))
http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
