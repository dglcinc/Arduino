#!/usr/bin/env python3
"""The factory flow against a simulated board: python3 test_factory.py [path to AirMaster.ino.uf2]. A stub picotool keeps the
board's flash in a file. Also runs the identity through the firmware's own parser (AirHandlerPico/test/unit_test)."""
import csv, json, os, stat, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import airmaster_factory as af

STUB = r'''#!/usr/bin/env python3
import json, os, struct, sys
S = os.environ["STUB_STATE"]
st = json.load(open(S))
a = sys.argv[1:]
if a[0] == "info":
    if not st["present"]: sys.exit(1)
    print("Device Information\n flash id: E661AABB"); sys.exit(0)
if a[0] == "save":
    frm, to, out = int(a[2], 16), int(a[3], 16), a[4]
    d = bytes.fromhex(st["flash"].get(str(frm), "ff" * 256))
    if st.get("corrupt") and st["saves"] >= 1: d = bytes(256)
    st["saves"] += 1
    open(out, "wb").write(d[:to - frm])
elif a[0] == "load":
    if st.get("load_fails"): print("no such luck", file=sys.stderr); sys.exit(3)
    d = open(a[1], "rb").read()
    n = len(d) // 512
    for i in range(n):
        b = d[i * 512:(i + 1) * 512]
        m0, m1, fl, ad, sz, no, tot, fam = struct.unpack("<8I", b[:32])
        assert (m0, m1) == (0x0A324655, 0x9E5D5157) and no == i and tot == n
        st["flash"][str(ad)] = b[32:32 + sz].hex()
    st["loads"] += 1
elif a[0] == "reboot":
    st["present"] = False
json.dump(st, open(S, "w"))
'''

def fake_firmware():
    out = b""
    for i in range(4):
        d = bytes([i]) * 256
        b = struct.pack("<8I", af.UF2_MAGIC0, af.UF2_MAGIC1, af.UF2_HAS_FAMILY, 0x10000000 + i * 256, 256, i, 4, 0xE48BFF56) + d
        out += b + bytes(508 - len(b)) + struct.pack("<I", af.UF2_END)
    return out

def main():
    fw = open(sys.argv[1], "rb").read() if len(sys.argv) > 1 else fake_firmware()
    t = tempfile.mkdtemp()
    tool = os.path.join(t, "picotool"); open(tool, "w").write(STUB); os.chmod(tool, 0o755)
    state = os.path.join(t, "state.json"); ledger = os.path.join(t, "cfg", "units.csv")
    os.environ["STUB_STATE"] = state
    def fresh(**kw):
        s = {"present": True, "flash": {}, "saves": 0, "loads": 0}; s.update(kw)
        json.dump(s, open(state, "w"))
    def load(): return json.load(open(state))
    b = af.Board(tool)

    def pool(n):
        rows = af.read_ledger(ledger)
        for _ in range(n):
            rows.append({"serial": af.next_serial(rows), "unit_id": af.new_unit_id(rows), "password": af.new_password(), "flashed": "", "firmware_sha256": "", "board": ""})
        af.write_ledger(ledger, rows)

    # No labels yet: refuses, and nothing is written to the board
    fresh()
    try:
        af.flash_one(b, fw, ledger, False); assert False
    except RuntimeError as e:
        assert "unused labels" in str(e)
    assert load()["loads"] == 0

    # A page of labels: private, and all unused
    pool(12)
    assert stat.S_IMODE(os.stat(ledger).st_mode) == 0o600 and stat.S_IMODE(os.stat(os.path.dirname(ledger)).st_mode) == 0o700
    rows = af.read_ledger(ledger)
    assert len(af.unused(rows)) == 12 and len({r["unit_id"] for r in rows}) == 12 and len({r["password"] for r in rows}) == 12

    # A new board takes the first label; the page's password is what lands on the board
    row, reused = af.flash_one(b, fw, ledger, False)
    assert not reused and row["serial"] == "AM-0001" and row["password"] == rows[0]["password"]
    s = load()
    page = bytes.fromhex(s["flash"][str(af.ID_ADDR)])
    assert af.parse_identity(page)["password"] == rows[0]["password"] and s["present"] is False
    assert len(af.unused(af.read_ledger(ledger))) == 11

    # The same board again: same label, none used
    s["present"] = True; s["saves"] = 0; json.dump(s, open(state, "w"))
    row, reused = af.flash_one(b, fw, ledger, False)
    assert reused and row["serial"] == "AM-0001" and len(af.unused(af.read_ledger(ledger))) == 11

    # A read-back that differs, or a failed load: the label stays free, board left in BOOTSEL
    fresh(corrupt=True)
    try:
        af.flash_one(b, fw, ledger, False); assert False
    except RuntimeError:
        pass
    assert len(af.unused(af.read_ledger(ledger))) == 11 and load()["present"]
    fresh(load_fails=True)
    try:
        af.flash_one(b, fw, ledger, False); assert False
    except subprocess.CalledProcessError:
        pass
    assert len(af.unused(af.read_ledger(ledger))) == 11

    # The next good board takes the label those failures left free
    fresh()
    row, reused = af.flash_one(b, fw, ledger, False)
    assert row["serial"] == "AM-0002" and not reused

    # A board from another ledger keeps its identity and becomes known here
    other = af.make_identity("AM-0900", "ABCD", "abcd-efgh-jkmn")
    fresh(flash={str(af.ID_ADDR): other.hex()})
    row, reused = af.flash_one(b, fw, ledger, False)
    assert reused and row["serial"] == "AM-0900" and any(r["serial"] == "AM-0900" for r in af.read_ledger(ledger))
    assert len(af.unused(af.read_ledger(ledger))) == 10

    # Firmware that overlaps the identity page, and a file that is not UF2
    over = struct.pack("<8I", af.UF2_MAGIC0, af.UF2_MAGIC1, af.UF2_HAS_FAMILY, af.ID_ADDR, 256, 0, 1, 0xE48BFF56) + bytes(256)
    over += bytes(508 - len(over)) + struct.pack("<I", af.UF2_END)
    for bad, why in ((over, "overlap"), (b"junk" * 128, "not uf2")):
        try:
            af.merge_uf2(bad, bytes(256)); assert False, why
        except ValueError:
            pass

    # Passwords: alphabet, length, and no repeats in a batch
    pws = {af.new_password() for _ in range(2000)}
    assert len(pws) == 2000 and all(len(p) == 14 and set(p) <= set(af.ALPHABET + "-") for p in pws)
    print("factory flow ok,", len(fw) // 512, "firmware blocks")

    # The firmware's parser agrees with this tool's page
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "AirHandlerPico")
    exe = os.path.join(t, "idcheck")
    code = ('#include "UnitCore.h"\nint main(int c,char**v){uint8_t p[256];unit::fromHex(v[1],p,256);auto i=unit::parseIdentity(p);'
            'printf("%d %s %s %s\\n",i.valid,i.serial,i.unitId,i.setupPass);}')
    cpp = os.path.join(t, "idcheck.cpp"); open(cpp, "w").write(code)
    subprocess.run(["c++", "-std=c++17", "-I" + src, cpp, "-o", exe], check=True)
    p = af.make_identity("AM-0123", "4F2A", "abcd-efgh-jkmn")
    assert subprocess.run([exe, p.hex()], capture_output=True, text=True).stdout.strip() == "1 AM-0123 4F2A abcd-efgh-jkmn"
    print("firmware parser agrees")

main()
