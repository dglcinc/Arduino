"""One AirMaster's label, full size: the generic label from pivac hardware/label/gen-label.py with the title block moved up
to make room for the unit's setup details and a QR code that joins its setup network. Every pointer and pin letter sits at
its part's true position, as on the generic label. Coordinates are millimetres from the box's centre, seen from outside
the base with the top panel up, y up. Needs segno for the QR code and Chrome for the PDF."""
import os, subprocess, tempfile
import segno

CHROME = os.environ.get("AIRMASTER_CHROME", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome")
HW, HH = 28.0, 29.5                # label half width and height
SY, NH, NX = 7.33, 3.7, 21.3       # screw height, notch half height, notch inner x
INK, GREY = "#111111", "#777777"
FONT = "Helvetica Neue, Helvetica, Arial, sans-serif"
MONO = "Menlo, Consolas, monospace"
LEDS = (("PWR", 1.5, "#18a848"), ("FAN", -2.5, "#e8b400"), ("Y1", -6.5, "#1f5fd8"), ("Y2", -10.5, "#1f5fd8"),
        ("W1", -14.5, "#d8261c"), ("W2", -18.5, "#d8261c"))
QR_SIZE, QR_LEFT, QR_TOP = 13.5, -26.5, -7.3     # 1.3 mm or more of white on every side: the code's quiet zone

def X(x): return x + HW
def Y(y): return HH - y

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def text(x, y, s, size, weight="normal", anchor="middle", fill=INK, font=FONT):
    return f'<text x="{X(x):.2f}" y="{Y(y):.2f}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}" fill="{fill}" font-family="{font}">{s}</text>'

def tri(x, y, d):
    s = 0.9
    p = {"up": [(x - s, y - 1.3), (x + s, y - 1.3), (x, y)], "down": [(x - s, y + 1.3), (x + s, y + 1.3), (x, y)],
         "right": [(x - 1.3, y - s), (x - 1.3, y + s), (x, y)]}[d]
    return '<polygon points="%s" fill="%s"/>' % (" ".join(f"{X(a):.2f},{Y(b):.2f}" for a, b in p), INK)

def outline():
    r, n = 3.0, 1.2
    top, bot = SY + NH, SY - NH
    p = [f"M {X(-HW + r):.2f} {Y(HH):.2f}", f"L {X(HW - r):.2f} {Y(HH):.2f}", f"A {r} {r} 0 0 1 {X(HW):.2f} {Y(HH - r):.2f}",
         f"L {X(HW):.2f} {Y(top):.2f}", f"L {X(NX + n):.2f} {Y(top):.2f}", f"A {n} {n} 0 0 0 {X(NX):.2f} {Y(top - n):.2f}",
         f"L {X(NX):.2f} {Y(bot + n):.2f}", f"A {n} {n} 0 0 0 {X(NX + n):.2f} {Y(bot):.2f}", f"L {X(HW):.2f} {Y(bot):.2f}",
         f"L {X(HW):.2f} {Y(-HH + r):.2f}", f"A {r} {r} 0 0 1 {X(HW - r):.2f} {Y(-HH):.2f}", f"L {X(-HW + r):.2f} {Y(-HH):.2f}",
         f"A {r} {r} 0 0 1 {X(-HW):.2f} {Y(-HH + r):.2f}", f"L {X(-HW):.2f} {Y(bot):.2f}", f"L {X(-NX - n):.2f} {Y(bot):.2f}",
         f"A {n} {n} 0 0 0 {X(-NX):.2f} {Y(bot + n):.2f}", f"L {X(-NX):.2f} {Y(top - n):.2f}", f"A {n} {n} 0 0 0 {X(-NX - n):.2f} {Y(top):.2f}",
         f"L {X(-HW):.2f} {Y(top):.2f}", f"L {X(-HW):.2f} {Y(HH - r):.2f}", f"A {r} {r} 0 0 1 {X(-HW + r):.2f} {Y(HH):.2f}", "Z"]
    return " ".join(p)

def monogram(cx, cy, scale):
    """hardware/dl-monogram.svg, ring and letters, without its white square."""
    return (f'<g transform="translate({X(cx):.2f} {Y(cy):.2f}) scale({scale})" fill="{INK}">'
            f'<circle r="4.425" fill="none" stroke="{INK}" stroke-width="0.35"/>'
            '<path d="M -2.35 1.01 L -2.35 -2.51 A 0.24 0.24 0 0 1 -2.11 -2.75 L -1.55 -2.75 A 2 2 0 0 1 -1.55 1.25 L -2.11 1.25 A 0.24 0.24 0 0 1 -2.35 1.01 Z M -1.55 -1.95 L -1.55 0.45 A 1.2 1.2 0 0 0 -1.55 -1.95 Z"/>'
            '<path d="M 0.8 -1.23 A 0.24 0.24 0 0 1 1.04 -1.47 L 1.36 -1.47 A 0.24 0.24 0 0 1 1.6 -1.23 L 1.6 1.59 A 0.14 0.14 0 0 0 1.74 1.73 L 2.793 1.73 A 0.24 0.24 0 0 1 3.033 1.97 L 3.033 2.29 A 0.24 0.24 0 0 1 2.793 2.53 L 1.04 2.53 A 0.24 0.24 0 0 1 0.8 2.29 Z"/></g>')

def wifi_qr_text(ssid, password):
    """The standard WiFi QR text. The unit id and the password alphabet hold none of the characters it would need to escape."""
    for c in ';,:\\"':
        if c in ssid or c in password:
            raise ValueError("needs escaping: " + c)
    return f"WIFI:T:WPA;S:{ssid};P:{password};;"

def qr_svg(payload):
    q = segno.make(payload, error="m", micro=False)
    rows = list(q.matrix)
    m = QR_SIZE / len(rows)
    d = "".join(f"M{X(QR_LEFT + c * m):.3f} {Y(QR_TOP) + r * m:.3f}h{m:.3f}v{m:.3f}h-{m:.3f}z"
                for r, row in enumerate(rows) for c, v in enumerate(row) if v)
    return f'<path d="{d}" fill="#000" shape-rendering="crispEdges"/>'

def art(serial, unit_id, password):
    ssid = "AirMaster-" + unit_id
    a = [f'<path d="{outline()}" fill="#ffffff"/>']
    # title block
    a += [monogram(-5.5, 12.6, 1.25), text(-5.5, -2.0, "AirMaster", 7.6, "bold"), text(-5.5, -5.4, "for Unico SMART controller", 2.6)]
    # this unit: scan the code, or type what it says
    a.append(qr_svg(wifi_qr_text(ssid, password)))
    tx = QR_LEFT + QR_SIZE + 1.5
    a += [text(tx, -9.0, "To set up, scan, or join WiFi", 1.9, anchor="start"),
          text(tx, -11.8, esc(ssid), 2.7, "bold", "start"),
          text(tx, -14.4, "Password", 1.9, anchor="start"),
          text(tx, -17.2, esc(password), 2.5, "bold", "start", font=MONO),
          text(tx, -20.0, "S/N " + esc(serial), 1.9, anchor="start", fill=GREY)]
    # top panel: 24 VAC with its true pin positions, the Pico's port, the Unico's USB
    a += [tri(-19.0, HH - 0.25, "up"), text(-19.0, 24.9, "24 VAC", 2.5, "bold"), text(-20.91, 22.2, "R", 2.2), text(-17.09, 22.2, "C", 2.2)]
    a += [tri(0, HH - 0.25, "up"), text(0, 24.9, "PROG", 2.5, "bold"), text(0, 22.2, "24 VAC off", 2.0)]
    a += [tri(19.8, HH - 0.25, "up"), text(19.8, 24.9, "UNICO", 2.5, "bold"), text(19.8, 22.2, "USB", 2.2)]
    # bottom panel: sensor plugs, pin letters at the true 2.5 mm pitch
    a += [tri(-20.0, -HH + 0.25, "down"), text(-20.0, -24.0, "AIR NTC", 2.5, "bold")]
    for x, s in ((-23.75, "S"), (-21.25, "&#8211;"), (-18.75, "R"), (-16.25, "&#8211;")):
        a.append(text(x, -26.6, s, 2.2))
    a += [tri(18.9, -HH + 0.25, "down"), text(18.9, -24.0, "1-WIRE", 2.5, "bold")]
    for x, s in ((16.4, "V"), (18.9, "D"), (21.4, "G")):
        a.append(text(x, -26.6, s, 2.2))
    # LEDs on the side wall
    for name, y, col in LEDS:
        a.append(tri(HW - 0.25, y, "right"))
        a.append(f'<circle cx="{X(24.4):.2f}" cy="{Y(y):.2f}" r="1.05" fill="{col}"/>')
        a.append(text(22.5, y - 0.8, name, 2.3, "bold", "end"))
    return "\n".join(a)

def svg(serial, unit_id, password, cut=True):
    w, h = 2 * HW, 2 * HH
    c = f'<path d="{outline()}" fill="none" stroke="{GREY}" stroke-width="0.1"/>' if cut else ""
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}mm" height="{h}mm" viewBox="0 0 {w} {h}">{art(serial, unit_id, password)}{c}</svg>'

def _chrome(html, pdf=None, png=None, size=None):
    fd, name = tempfile.mkstemp(suffix=".html")
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(html)
    args = [CHROME, "--headless=new", "--disable-gpu", "--no-pdf-header-footer", "--force-device-scale-factor=4"]
    if pdf:
        args.append("--print-to-pdf=" + pdf)
    if png:
        args += ["--screenshot=" + png, "--window-size=%d,%d" % size]
    try:
        subprocess.run(args + ["file://" + name], check=True, capture_output=True, timeout=120)
    finally:
        os.unlink(name)

def _page(w, h, body):
    return f'<!doctype html><html><head><style>@page{{size:{w} {h};margin:0}}html,body{{margin:0;padding:0;background:#fff}}svg{{display:block}}</style></head><body>{body}</body></html>'

def sheet_pdf(units, path):
    """Letter pages of 12 labels, in serial order, with a scale check. units: dicts with serial, unit_id, password."""
    w, h = 2 * HW, 2 * HH
    left, top, gap_x, gap_y = 18.0, 7.0, 6.0, 4.0
    spots = [(left + c * (w + gap_x), top + r * (h + gap_y)) for r in range(4) for c in range(3)]
    bar = ('<div style="position:absolute;left:18mm;top:257mm;font:8pt Helvetica,Arial,sans-serif">'
           '<div style="width:50mm;height:1.5mm;background:#111"></div>'
           '<div style="margin-top:1mm">Print at 100 %: this bar must measure 50 mm. Cut on the grey line, including the side notches, which clear the cover screws.</div></div>')
    pages = []
    for i in range(0, len(units), 12):
        cells = "".join(f'<div style="position:absolute;left:{x}mm;top:{y}mm">{svg(u["serial"], u["unit_id"], u["password"])}</div>'
                        for (x, y), u in zip(spots, units[i:i + 12]))
        pages.append(f'<div style="position:relative;width:8.5in;height:11in;overflow:hidden;page-break-after:always">{cells}{bar}</div>')
    _chrome(_page("8.5in", "11in", "".join(pages)), pdf=path)

def preview_png(u, path, k=10):
    w, h = 2 * HW, 2 * HH
    big = f'<div style="padding:{k*2}px;background:#fff">' + svg(u["serial"], u["unit_id"], u["password"]).replace(f'width="{w}mm" height="{h}mm"', f'width="{w*k}" height="{h*k}"') + '</div>'
    _chrome(_page("auto", "auto", big), png=path, size=(int(w * k + 4 * k), int(h * k + 4 * k)))
