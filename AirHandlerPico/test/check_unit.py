#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright David Lewis
"""Compares unit_test's SHA-256, password hash and CRC-32 with Python's. Run from AirHandlerPico/test."""
import hashlib, subprocess, sys, zlib, tempfile, os
t = os.path.join(tempfile.gettempdir(), "am_unit_test")
subprocess.run(["c++", "-std=c++17", "-Wall", "-I..", "unit_test.cpp", "-o", t], check=True)
subprocess.run([t], check=True)
out = subprocess.run([t, "vectors"], check=True, capture_output=True, text=True).stdout.splitlines()
n = 0
for line in out:
    kind, val, *rest = line.split(" ", 2)
    if kind == "sha256":
        msg = rest[0] if rest else ""
        assert hashlib.sha256(msg.encode()).hexdigest() == val, line
    elif kind == "pw":
        salt, pw = "00112233445566778899aabbccddeeff", "hunter2 pw"
        d = hashlib.sha256((salt + pw).encode()).digest()
        for _ in range(5000):
            d = hashlib.sha256(d).digest()
        assert d.hex() == val, line
    elif kind == "crc":
        assert "%08x" % zlib.crc32(b"123456789") == val, line
    n += 1
print("python agrees on", n, "values")
