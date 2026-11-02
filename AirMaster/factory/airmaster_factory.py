#!/usr/bin/env python3
"""Print AirMaster labels ahead of time, then flash boards and match each to the next unused label.

`sheet` makes a Letter page of 12 labels and gives each its own serial number, unit id for the setup network and random
setup password. `flash` merges the next unused identity into the firmware image, loads it, reads it back from the board
and tells you which label to stick on. A board that already has an identity keeps it, so re-flashing never strands a label.
A failed flash leaves its label unused for the next board.

  python3 -m venv ~/.config/airmaster/venv && ~/.config/airmaster/venv/bin/pip install segno
  ~/.config/airmaster/venv/bin/python airmaster_factory.py sheet --print
  ~/.config/airmaster/venv/bin/python airmaster_factory.py flash --uf2 build/AirMaster.ino.uf2 --count 5
  ~/.config/airmaster/venv/bin/python airmaster_factory.py labels AM-0003       # that label again, on a sheet
  ~/.config/airmaster/venv/bin/python airmaster_factory.py list

The ledger (default ~/.config/airmaster/units.csv) and the label PDFs hold every unit's password, so they stay outside the
repo, readable only by you. Keep a backup of the ledger, because unused labels cannot be matched to a board without it.
This tool prints a password nowhere but the labels and the ledger."""
import argparse, csv, glob, hashlib, os, secrets, shutil, struct, subprocess, sys, tempfile, time, zlib

UF2_MAGIC0, UF2_MAGIC1, UF2_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_HAS_FAMILY = 0x2000
ID_ADDR = 0x101FF000            # the flash's last sector: the core's EEPROM area, which this firmware does not use
ID_LEN = 256
ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"   # no 0, 1, i, l, o: a person may have to type it from the label
FIELDS = ["serial", "unit_id", "password", "flashed", "firmware_sha256", "board"]
POWER_NOTE = "Unplug 24 VAC and the USB-A cable. Hold BOOTSEL, plug USB into the micro-USB port, and let go."


# --- the identity: AirHandlerPico/UnitCore.h reads this ---------------------------------------------

def make_identity(serial, unit_id, password):
    for s, n in ((serial, 16), (unit_id, 8), (password, 24)):
        if len(s.encode()) >= n:
            raise ValueError("too long: " + s[:3] + "...")
    if len(password) < 8:
        raise ValueError("WPA2 needs at least 8 characters")
    p = bytearray(ID_LEN)
    p[0:4] = b"AMID"
    p[4] = 1
    p[8:8 + len(serial)] = serial.encode()
    p[24:24 + len(unit_id)] = unit_id.encode()
    p[32:32 + len(password)] = password.encode()
    p[56:60] = struct.pack("<I", zlib.crc32(bytes(p[:56])))
    return bytes(p)


def parse_identity(p):
    if len(p) < 60 or p[:4] != b"AMID" or p[4] != 1 or struct.unpack("<I", p[56:60])[0] != zlib.crc32(p[:56]):
        return None
    txt = lambda a, b: p[a:b].split(b"\0")[0].decode()
    return {"serial": txt(8, 24), "unit_id": txt(24, 32), "password": txt(32, 56)}


def new_password():
    return "-".join("".join(secrets.choice(ALPHABET) for _ in range(4)) for _ in range(3))


# --- UF2 ----------------------------------------------------------------------------------------------

def uf2_blocks(data):
    """(address, payload, flags, family) for each 512-byte block; raises on anything that is not UF2."""
    if not data or len(data) % 512:
        raise ValueError("not a UF2 file")
    out = []
    for i in range(0, len(data), 512):
        b = data[i:i + 512]
        m0, m1, flags, addr, size, _no, _n, family = struct.unpack("<8I", b[:32])
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1 or struct.unpack("<I", b[508:512])[0] != UF2_END or size > 476:
            raise ValueError("not a UF2 file")
        out.append((addr, b[32:32 + size], flags, family))
    return out


def merge_uf2(firmware, page, addr=ID_ADDR):
    blocks = uf2_blocks(firmware)
    if any(a + len(d) > addr and a < addr + ID_LEN for a, d, _, _ in blocks):
        raise ValueError("the firmware overlaps the identity page")
    flags, family = blocks[0][2], blocks[0][3]
    if not flags & UF2_HAS_FAMILY:
        raise ValueError("the firmware has no family id")
    blocks.append((addr, page, flags, family))
    out = bytearray()
    for n, (a, d, f, fam) in enumerate(blocks):
        b = struct.pack("<8I", UF2_MAGIC0, UF2_MAGIC1, f, a, len(d), n, len(blocks), fam) + d
        out += b + bytes(508 - len(b)) + struct.pack("<I", UF2_END)
    return bytes(out)


# --- the ledger -----------------------------------------------------------------------------------------

def private_dir(path):
    os.makedirs(path, mode=0o700, exist_ok=True)
    os.chmod(path, 0o700)


def read_ledger(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def write_ledger(path, rows):
    """Replaces the ledger in one step, so a crash leaves the old file or the new one."""
    private_dir(os.path.dirname(path))
    tmp = path + ".tmp"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", newline="") as f:
        w = csv.DictWriter(f, FIELDS)
        w.writeheader()
        w.writerows(rows)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def unused(rows):
    return [r for r in rows if not r["flashed"]]


def next_serial(rows):
    n = 0
    for r in rows:
        try:
            n = max(n, int(r["serial"].split("-")[1]))
        except (IndexError, ValueError):
            pass
    return "AM-%04d" % (n + 1)


def new_unit_id(rows):
    used = {r["unit_id"] for r in rows}
    while True:
        u = secrets.token_hex(2).upper()
        if u not in used:
            return u


# --- picotool ---------------------------------------------------------------------------------------------

def find_picotool():
    p = os.environ.get("AIRMASTER_PICOTOOL") or shutil.which("picotool")
    if p:
        return p
    found = sorted(glob.glob(os.path.expanduser("~/Library/Arduino15/packages/rp2040/tools/pqt-picotool/*/picotool")))
    if not found:
        sys.exit("picotool not found. Install the arduino-pico core, or set AIRMASTER_PICOTOOL.")
    return found[-1]


class Board:
    def __init__(self, tool):
        self.tool = tool

    def _run(self, *args, check=True):
        return subprocess.run([self.tool, *args], capture_output=True, text=True, timeout=180, check=check)

    def wait(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if self._run("info", "-d", check=False).returncode == 0:
                return True
            time.sleep(1)
        return False

    def board_id(self):
        r = self._run("info", "-d", check=False)
        for line in r.stdout.splitlines():
            if "flash id" in line.lower():
                return line.split(":", 1)[1].strip()
        return ""

    def read_page(self, workdir):
        out = os.path.join(workdir, "page.bin")
        self._run("save", "-r", "0x%08X" % ID_ADDR, "0x%08X" % (ID_ADDR + ID_LEN), out, "-t", "bin")
        with open(out, "rb") as f:
            return f.read()

    def load(self, image, workdir):
        path = os.path.join(workdir, "image.uf2")
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(image)
        self._run("load", path, "-t", "uf2", "-v")

    def reboot(self):
        self._run("reboot")


# --- the commands -----------------------------------------------------------------------------------------

def flash_one(board, firmware, ledger, force_new):
    """Flashes the connected board. Returns its ledger row and whether it kept an identity; raises with the ledger unchanged."""
    workdir = tempfile.mkdtemp(prefix="airmaster-")
    os.chmod(workdir, 0o700)
    try:
        rows = read_ledger(ledger)
        have = None if force_new else parse_identity(board.read_page(workdir))
        if have:
            row = next((r for r in rows if r["serial"] == have["serial"] and r["unit_id"] == have["unit_id"]), None)
            if not row:                    # flashed from another ledger: known from here on
                row = dict(have, flashed="", firmware_sha256="", board="")
                rows.append(row)
        else:
            free = unused(rows)
            if not free:
                raise RuntimeError("no unused labels left. Run the sheet command and print another page.")
            row = free[0]
        page = make_identity(row["serial"], row["unit_id"], row["password"])
        board.load(merge_uf2(firmware, page), workdir)
        if board.read_page(workdir)[:ID_LEN] != page:
            raise RuntimeError("the identity read back from the board differs from what was written")
        row.update(flashed=time.strftime("%Y-%m-%d %H:%M"), firmware_sha256=hashlib.sha256(firmware).hexdigest()[:16], board=board.board_id())
        write_ledger(ledger, rows)         # before the board leaves BOOTSEL; a re-flash reads the identity back anyway
        board.reboot()
        return row, bool(have)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def write_labels(units, ledger, send_to_printer, printer):
    import label
    folder = os.path.join(os.path.dirname(ledger), "labels")
    private_dir(folder)
    path = os.path.join(folder, "%s_to_%s.pdf" % (units[0]["serial"], units[-1]["serial"]))
    label.sheet_pdf(units, path)
    os.chmod(path, 0o600)
    print("labels: " + path)
    if send_to_printer:
        cmd = ["lp", "-o", "fit-to-page=false", "-o", "scaling=100"] + (["-d", printer] if printer else []) + [path]
        subprocess.run(cmd, check=True)
        print("sent to " + (printer or "the default printer") + ". Check the 50 mm bar.")
    return path


def cmd_sheet(a):
    """New identities for a full page, recorded before the page exists, so no printed label is ever unknown to the ledger."""
    rows = read_ledger(a.ledger)
    new = []
    for _ in range(a.count):
        r = {"serial": next_serial(rows + new), "unit_id": new_unit_id(rows + new), "password": new_password(), "flashed": "", "firmware_sha256": "", "board": ""}
        make_identity(r["serial"], r["unit_id"], r["password"])
        new.append(r)
    write_ledger(a.ledger, rows + new)
    write_labels(new, a.ledger, a.print_labels, a.printer)
    print("%d new labels, %s to %s; %d unused in all." % (len(new), new[0]["serial"], new[-1]["serial"], len(unused(rows + new))))
    return 0


def cmd_flash(a):
    with open(a.uf2, "rb") as f:
        firmware = f.read()
    uf2_blocks(firmware)
    board = Board(find_picotool())
    done = 0
    for i in range(a.count):
        left = len(unused(read_ledger(a.ledger)))
        print("\nUnit %d of %d, %d unused labels. %s" % (i + 1, a.count, left, POWER_NOTE), flush=True)
        if not board.wait(a.wait):
            print("no board in BOOTSEL after %d s; stopping." % a.wait)
            break
        try:
            row, reused = flash_one(board, firmware, a.ledger, a.new_identity)
        except (RuntimeError, ValueError, subprocess.CalledProcessError) as e:
            detail = e.stderr.strip() if isinstance(e, subprocess.CalledProcessError) and e.stderr else str(e)
            print("FAILED, and no label used: " + detail)
            break
        done += 1
        print("%s label %s  (AirMaster-%s). Unplug the board and stick that label on its box." % ("It already had an identity: use" if reused else "Use", row["serial"], row["unit_id"]), flush=True)
        if i + 1 < a.count:
            while board.wait(1):           # the old board may still be on its way out of BOOTSEL
                time.sleep(1)
    return 0 if done == a.count else 1


def cmd_labels(a):
    rows = read_ledger(a.ledger)
    units = [r for r in rows if r["serial"] in a.serials] if a.serials else rows
    missing = set(a.serials) - {r["serial"] for r in units}
    if missing or not units:
        sys.exit("not in the ledger: " + (", ".join(sorted(missing)) or "no units yet"))
    write_labels(units, a.ledger, a.print_labels, a.printer)
    return 0


def cmd_list(a):
    for r in read_ledger(a.ledger):
        print(r["serial"], "AirMaster-" + r["unit_id"], r["flashed"] or "unused", r["firmware_sha256"], r["board"])
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Flash AirMaster boards and print their labels.")
    ap.add_argument("--ledger", default=os.path.expanduser("~/.config/airmaster/units.csv"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("sheet", help="make new identities and a page of their labels")
    n.add_argument("--count", type=int, default=12, help="labels to make; a page holds 12")
    n.add_argument("--print", dest="print_labels", action="store_true", help="send the sheet to the printer")
    n.add_argument("--printer")
    n.set_defaults(func=cmd_sheet)
    f = sub.add_parser("flash", help="flash boards one after another, each with the next unused label")
    f.add_argument("--uf2", required=True, help="AirMaster.ino.uf2 from the build")
    f.add_argument("--count", type=int, default=1)
    f.add_argument("--wait", type=int, default=180, help="seconds to wait for each board")
    f.add_argument("--new-identity", action="store_true", help="give the board the next unused label even if it has an identity; its old label stops working")
    f.set_defaults(func=cmd_flash)
    l = sub.add_parser("labels", help="print labels again, from the ledger")
    l.add_argument("serials", nargs="*", help="serial numbers; none means every unit")
    l.add_argument("--print", dest="print_labels", action="store_true")
    l.add_argument("--printer")
    l.set_defaults(func=cmd_labels)
    s = sub.add_parser("list", help="list the units, without their passwords")
    s.set_defaults(func=cmd_list)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
