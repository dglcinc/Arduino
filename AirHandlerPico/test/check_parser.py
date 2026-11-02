#!/usr/bin/env python3
"""Compares ScbParser.h with pivac's unico_scb_protocol.py on the same random bus traffic: every frame length,
PC and log frames, motor-bus noise and corrupted frames. Usage: check_parser.py ./parser_test [path to pivac]"""
import os, random, subprocess, sys
sys.path.insert(0, sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/github/pivac"))
import importlib.util
spec = importlib.util.spec_from_file_location("proto", os.path.join(sys.path[0], "pivac", "unico_scb_protocol.py"))
proto = importlib.util.module_from_spec(spec); spec.loader.exec_module(proto)
R = random.Random(20261031)

def status(n):
    level = R.choice((20, 23))
    calls = R.randrange(64)
    if n == 9:
        fw = R.choice((21, 30, 33))
        return proto.with_check(bytes([1, 0x83, R.choice((0, 32)), fw, calls << 2 | R.randrange(4), R.randrange(256), R.randrange(256)]))
    b = n == 37
    body = [1, 0x83, R.choice((0, 32)), level, calls << 2 | R.randrange(4)]
    body += [0, 7] if b else [7]
    for _ in range(14):
        v = R.randrange(100, 1800); body += [v >> 8, v & 255]
    body += [R.randrange(256) for _ in range(n - 2 - len(body))]
    return proto.with_check(bytes(body))

stream, want = bytearray(), []
for _ in range(4000):
    kind = R.random()
    if kind < 0.70:
        f = status(R.choice((9, 36, 36, 36, 37, 51)))
        stream += f
    elif kind < 0.78:
        stream += proto.with_check(bytes([1, 0x83, 0, proto.PC_TAG] + [R.randrange(256) for _ in range(R.choice((25, 37)))]))
    elif kind < 0.84:
        stream += proto.with_check(bytes([1, 0x84] + [R.randrange(256) for _ in range(98)]))
    elif kind < 0.92:
        stream += bytes(R.randrange(256) for _ in range(R.randrange(1, 30)))
    else:
        f = bytearray(status(36)); f[R.randrange(5, 34)] ^= 0x55
        stream += f

frames, tail = proto.split_frames(bytes(stream))
air, stored, cfm, rpm = 0, [0] * 6, 0, 0
for k, f in frames:
    if k != "status":
        continue
    d = proto.decode_status(f)
    c = "".join("1" if m in d["calls"] else "0" for m in proto.MODES)
    if "actual_cfm" in d:
        air = 1; stored = [d["cfm"][m] for m in proto.MODES]; cfm, rpm = d["actual_cfm"], d["actual_rpm"]
    want.append("%d %s %d %s %d %d" % (d["flags"], c, air, " ".join(map(str, stored)), cfm, rpm))

r = subprocess.run([sys.argv[1]], input=bytes(stream), capture_output=True)
got = r.stdout.decode().splitlines()
print("python %d status frames, C++ lines %d; %s" % (len(want), len(got), r.stderr.decode().strip()))
bad = [i for i, (a, b) in enumerate(zip(want, got)) if a != b]
print("first differences:", [(want[i], got[i]) for i in bad[:3]])
assert len(want) == len(got) and not bad, "parser differs from the Python"
print("PASS")
