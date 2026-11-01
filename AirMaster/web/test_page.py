#!/usr/bin/env python3
"""Drives index.html against sim.py in headless Chrome, at phone size. Needs `pip install playwright` and Chrome itself:
python3 test_page.py [folder for screenshots]. Exercises first setup, recovery, login, a settings change, its limits,
restore, sensors and the firmware upload, and fails on any browser error."""
import os, subprocess, sys, tempfile, time, urllib.request
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
SHOTS = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
PORTS = {"setup": 8771, "recovery": 8772, "fallback": 8773, "normal": 8774}
sims = [subprocess.Popen([sys.executable, os.path.join(HERE, "sim.py"), m, str(p)], stdout=subprocess.DEVNULL) for m, p in PORTS.items()]
def url(m): return "http://localhost:%d/" % PORTS[m]
for m in PORTS:
    for _ in range(50):
        try:
            urllib.request.urlopen(url(m) + "data"); break
        except OSError:
            time.sleep(0.1)

errors, checks = [], 0
def ok(cond, what):
    global checks
    checks += 1
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        errors.append(what)

try:
    with sync_playwright() as pw:
        br = pw.chromium.launch(channel="chrome", headless=True)
        def page():
            ctx = br.new_context(viewport={"width": 390, "height": 844})
            p = ctx.new_page()
            p.set_default_timeout(15000)
            p.on("pageerror", lambda e: errors.append("page error: %s" % e))
            p.on("console", lambda c: errors.append("console: " + c.text) if c.type == "error" and not any(x in c.text for x in ("400", "401", "403")) else None)
            return p
        msg = lambda p: p.locator(".msg").first.inner_text()

        # first setup
        p = page(); p.goto(url("setup"))
        p.get_by_role("button", name="Save and connect").wait_for()
        opts = p.locator("select option").all_inner_texts()
        ok(any('<b>x</b>' in o for o in opts) and p.locator("main b").count() == 0, "a network name with markup shows as text")
        ok(p.locator("nav").is_hidden(), "no tabs during setup")
        p.screenshot(path=SHOTS + "/setup.png", full_page=True)
        save = p.get_by_role("button", name="Save and connect")
        save.click(); ok("Choose a WiFi" in msg(p), "setup needs a network")
        p.locator("select").select_option("redux-test")
        pwd = p.locator("input[type=password]")
        pwd.nth(0).fill("wifipass99"); pwd.nth(1).fill("short"); pwd.nth(2).fill("short"); save.click()
        p.get_by_text("at least 8 characters").wait_for(); ok(True, "short admin password refused")
        pwd.nth(1).fill("adminpass1"); pwd.nth(2).fill("different"); save.click()
        p.get_by_text("differ").wait_for(); ok(True, "mismatched admin passwords refused")
        p.locator('input[placeholder="Master Bedroom"]').fill("Kids Room"); pwd.nth(2).fill("adminpass1"); save.click()
        p.locator(".addr").wait_for()
        ok(p.locator(".addr").inner_text() == "http://kids-room.local", "setup ends by showing the address")
        ok("AirMaster-4F2A" in p.locator("main").inner_text(), "and says what a failed join looks like")
        p.screenshot(path=SHOTS + "/joining.png", full_page=True)

        # recovery and fallback keep what is saved when left blank, and offer the readings
        for m in ("recovery", "fallback"):
            p = page(); p.goto(url(m))
            p.get_by_role("button", name="Save and connect").wait_for()
            ok(p.get_by_role("button", name="Skip, and see the readings").count() == 1, m + " can skip to the readings")
        p.get_by_role("button", name="Save and connect").click(); p.locator(".addr").wait_for()
        ok("Saved" in p.locator("h2").first.inner_text(), "fallback saves with every field blank")
        p = page(); p.goto(url("recovery")); p.get_by_role("button", name="Skip, and see the readings").click()
        p.locator("nav").wait_for(); ok(True, "skip shows the tabs")

        # status: open to anyone
        p = page(); p.goto(url("normal"))
        p.get_by_text("Airflow, CFM").wait_for(); p.wait_for_function("document.querySelector('.chip.on')")
        ok(p.locator(".chip.on").all_inner_texts() == ["Fan", "Cool 1"], "calls light their chips")
        body = p.locator("main").inner_text()
        ok("49.8 °F" in body and "5.6 °F" in body and "not calibrated" in body, "temperatures, coil difference and the calibration warning")
        p.screenshot(path=SHOTS + "/status.png", full_page=True)

        # settings: login, limits, change, restore
        p.locator("nav button[data-t=settings]").click()
        lg = p.get_by_role("button", name="Log in"); lg.wait_for()
        p.locator("input[type=password]").fill("wrong"); lg.click()
        p.get_by_text("Wrong password").wait_for(); ok(True, "wrong password refused")
        p.locator("input[type=password]").fill("testpass1"); lg.click()
        sv = p.get_by_role("button", name="Save changes"); sv.wait_for()
        g = p.locator("tbody input").first
        ok(g.input_value() == "300", "settings read from the board")
        ok("750 CFM" in p.locator("main").inner_text(), "the model's limit is shown")
        p.screenshot(path=SHOTS + "/settings.png", full_page=True)
        g.fill("9999"); sv.click(); p.locator(".msg.bad").wait_for(); ok("Fan only" in msg(p), "over the model's limit refused in the page")
        g.fill("300"); sv.click(); p.get_by_text("Nothing has changed").wait_for(); ok(True, "no change, nothing sent")
        g.fill("325"); sv.click(); y = p.get_by_role("button", name="Yes, send it"); y.wait_for()
        ok("cfm.G: 300 to 325" in p.locator("main").inner_text(), "the confirmation lists the change")
        p.screenshot(path=SHOTS + "/confirm.png", full_page=True)
        p.get_by_role("button", name="Cancel").click(); ok(y.count() == 0, "cancel sends nothing")
        sv.click(); y.click(); p.locator(".msg.ok").wait_for(); ok(p.locator("tbody input").first.input_value() == "325", "the change is stored and read back")
        p.get_by_role("button", name="Restore original settings").click(); y.click()
        p.get_by_text("The original settings are back").wait_for(); ok(p.locator("tbody input").first.input_value() == "300", "restore puts back the first value read")

        # unit
        p.locator("nav button[data-t=unit]").click()
        p.get_by_text("Firmware update").wait_for()
        ok("master-bedroom.local" in p.locator(".addr").inner_text() and "AM-0001" in p.locator("main").inner_text(), "address and serial shown")
        p.screenshot(path=SHOTS + "/unit.png", full_page=True)
        sens = p.locator("section", has_text="Water supply probe")
        a, b = sens.locator("select").nth(0), sens.locator("select").nth(1)
        keep = a.input_value(), b.input_value()
        a.select_option(keep[1]); sens.get_by_role("button", name="Save sensors").click()
        p.get_by_text("different probes").wait_for(); ok(True, "the same probe twice refused")
        a.select_option(keep[1]); b.select_option(keep[0])
        p.locator("label", has_text="Air return").locator("input").fill("-0.25"); sens.get_by_role("button", name="Save sensors").click()
        p.locator(".msg.ok").wait_for()
        info = p.evaluate("fetch('/api/info').then(r=>r.json())")
        ok(info["offsets"]["aret"] == -0.25 and next(x for x in info["probes"] if x["role"] == "sup")["rom"] == keep[1], "swapped probes and an offset saved")
        ok(p.evaluate("fetch('/data').then(r=>r.json())")["cal"] == 1, "the reading says it is calibrated")
        f = tempfile.NamedTemporaryFile(suffix=".bin"); f.write(b"x" * 2048); f.flush()
        p.locator("input[type=file]").set_input_files(f.name); p.get_by_role("button", name="Install").click()
        p.get_by_text("restarting").wait_for(); ok(True, "firmware upload then restart message")

        # the rules a page cannot get round
        p = page(); p.goto(url("normal"))
        p.get_by_text("Airflow, CFM").wait_for()
        r = p.evaluate("fetch('/api/set',{method:'POST',body:'cfm.G=400',headers:{'X-AirMaster':'1','Content-Type':'application/x-www-form-urlencoded'}}).then(r=>r.status)")
        ok(r == 401, "a change without logging in is refused")
        r = p.evaluate("fetch('/api/login',{method:'POST',body:'password=testpass1',headers:{'Content-Type':'application/x-www-form-urlencoded'}}).then(r=>r.status)")
        ok(r == 403, "a POST without the page's header is refused")
        br.close()
finally:
    for s in sims:
        s.terminate()
print("%d checks, %d problems" % (checks, len(errors)))
for e in errors:
    print("  " + e)
sys.exit(1 if errors else 0)
