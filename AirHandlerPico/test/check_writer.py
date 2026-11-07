#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright David Lewis
"""Compares ScbSettings.h with pivac's unico_scb_protocol.py and scripts/unico-scb.py's limits on random 51-byte frames and
changes. Usage: check_writer.py ./writer_test [path to pivac]"""
import importlib.util, os, random, subprocess, sys
root = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/github/pivac")
spec = importlib.util.spec_from_file_location("proto", os.path.join(root, "pivac", "unico_scb_protocol.py"))
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
R = random.Random(20261101)
AIR = {"percent": 255, "blower": None, "sqft": 30000, "occupants": 51, "runmin": 60, "interval": 24, "bedrooms": 50}

def limit_ok(want, model):
    cap, floor = p.CFM_MAX[model], p.RPM_MIN.get(model, 400)
    if any(not 0 < v <= cap for v in want["cfm"].values()): return False
    if any(not floor <= v <= p.RPM_MAX for v in want["rpm"].values()): return False
    return all(0 <= want["air"][k] <= (l if l is not None else cap) for k, l in AIR.items())

def frame():
    def u(v): return [v >> 8, v & 255]
    pl = R.choice((6, 12))
    body = [1, 0x83, R.randrange(256) | (0x80 if R.random() < 0.9 else 0), 23, R.randrange(64) << 2 | R.randrange(4), pl]
    for _ in range(6): body += u(R.randrange(100, 1500))
    for _ in range(6): body += u(R.randrange(400, 1800))
    body += u(R.randrange(100, 1500)) + u(R.randrange(100, 1800))
    body += [R.randrange(256)] + u(R.randrange(0, 1500)) + [R.randrange(256)]
    body += u(R.randrange(65536)) + u(R.randrange(0, 30000))
    body += [R.randrange(256), R.randrange(0, 61), R.randrange(0, 25), R.randrange(256), R.randrange(256)] + u(R.randrange(65536))
    assert len(body) == 49
    return p.with_check(bytes(body))

lines, cases = [], []
for _ in range(3000):
    f = frame()
    ch, py = [], {"cfm": {}, "rpm": {}, "air": {}, "auto": {}}
    for _ in range(R.randrange(0, 5)):
        k = R.randrange(7)
        if k == 0:
            m = R.choice(p.MODES); v = R.randrange(1, 1600); ch.append("cfm.%s=%d" % (m, v)); py["cfm"][m] = v
        elif k == 1:
            m = R.choice(p.MODES); v = R.randrange(300, 1900); ch.append("rpm.%s=%d" % (m.lower(), v)); py["rpm"][m] = v
        elif k == 2:
            n = R.choice(("percent", "sqft", "occupants", "runmin", "interval", "bedrooms"))
            v = R.randrange(0, AIR[n] + 1); ch.append("air.%s=%d" % (n, v)); py["air"][n] = v
        elif k == 3:
            v = R.randrange(0, 1600); ch.append("air.blower=%d" % v); py["air"]["blower"] = v
        elif k == 4:
            v = R.choice(("on", "off")); ch.append("air.eac=" + v); py["air"]["eac"] = v == "on"
        elif k == 5:
            v = R.choice(("auto", "fixed")); ch.append("air.mode=" + v); py["air"]["mode"] = v
        else:
            v = R.choice(("poweropen", "powerclose")); ch.append("damper=" + v); py["damper"] = v
    lines.append(f.hex() + " " + " ".join(ch))
    cases.append((f, py))

out = subprocess.run([sys.argv[1]], input=("\n".join(lines) + "\n").encode(), capture_output=True).stdout.decode().splitlines()
assert len(out) == len(cases), (len(out), len(cases))
refused = limited = 0
for (f, py), line in zip(cases, out):
    try:
        st = p.stored_settings(f)
    except ValueError:
        assert line == "refuse", line
        refused += 1
        continue
    want = p.merge(st, py)
    model = st["model"]
    a, i, verdict = line.split()
    assert a == p.answer_frame(f, want).hex(), ("answer", f.hex(), py, a)
    assert i == p.idle_frame(f).hex(), ("idle", f.hex())
    ok = limit_ok(want, model)
    limited += not ok
    assert verdict == ("ok" if ok else "limit"), ("limit", f.hex(), py, verdict, model)
print("compared %d frames: %d refused for unknown Air Cycle mode, %d over a limit" % (len(cases), refused, limited))
print("PASS")
