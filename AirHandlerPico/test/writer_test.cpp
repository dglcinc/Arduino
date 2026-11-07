// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// Two checks away from the hardware. "frames" reads lines of a 51-byte frame in hex plus key=value changes and prints
// the C++ results for check_writer.py to compare with the Python. "sim" runs ScbSetJob against a simulated SCB.
#include <cstdio>
#include <cstring>
#include <string>
#include <iostream>
#include <sstream>
#include <vector>
#include "ScbParser.h"
#include "ScbSetJob.h"

static std::vector<uint8_t> bytes(const std::string &h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back(strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}
static void hex(const uint8_t *f, size_t n) { for (size_t i = 0; i < n; i++) printf("%02x", f[i]); }

static int frames() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream is(line);
    std::string h, kv;
    is >> h;
    auto f = bytes(h);
    ScbSettings s;
    bool ok = scb::stored(f.data(), f.size(), s);
    if (!ok) { puts("refuse"); continue; }
    bool good = true;
    while (is >> kv) {
      size_t e = kv.find('=');
      good = good && scb::applyChange(s, kv.substr(0, e).c_str(), kv.substr(e + 1).c_str());
    }
    if (!good) { puts("badkey"); continue; }
    uint8_t o[43], idle[43];
    scb::answerFrame(f.data(), s, o);
    scb::idleFrame(f.data(), idle);
    const char *bad = scb::checkLimits(s, f.data());
    hex(o, 43); putchar(' '); hex(idle, 43); printf(" %s\n", bad ? "limit" : "ok");
  }
  return 0;
}

// --- the simulated SCB: 36 bytes until answered, then 51 for 9 s; stores what a 43-byte frame requests
struct Sim {
  ScbSettings st;
  uint8_t model = 0x01;                // a 2430
  uint32_t lastAnswer = 0xFFFFFFFF;
  int sentIdle = 0, sentAnswer = 0;
  bool storeWrites = true;
  std::vector<uint8_t> out;
  void frame(bool lng, uint8_t *f, size_t &n) {
    f[0] = 1; f[1] = 0x83; f[2] = 0xA0; f[3] = 23; f[4] = 0xFC | model; f[5] = 6;
    for (int k = 0; k < 6; k++) { f[6 + 2 * k] = st.cfm[k] >> 8; f[7 + 2 * k] = st.cfm[k]; f[18 + 2 * k] = st.rpm[k] >> 8; f[19 + 2 * k] = st.rpm[k]; }
    f[30] = 1; f[31] = 44; f[32] = 4; f[33] = 0;
    if (!lng) { n = 36; scb::withCheck(f, 34); return; }
    uint16_t sw = (st.energySaver ? 0x2000 : 0) | (st.powerOpen ? 0x4000 : 0);
    f[34] = st.percent; f[35] = st.blower >> 8; f[36] = st.blower; f[37] = 90; f[38] = sw >> 8; f[39] = sw;
    f[40] = st.sqft >> 8; f[41] = st.sqft; f[42] = st.occupants; f[43] = st.runmin; f[44] = st.interval;
    f[45] = st.bedrooms | (st.eac ? 0x40 : 0) | (st.autoMode ? 0x80 : 0);
    f[46] = 21; f[47] = 0; f[48] = 4;
    n = 51; scb::withCheck(f, 49);
  }
  void take(const uint8_t *o, size_t n, uint32_t now) {
    lastAnswer = now;
    if (o[4] & 0xFC) { puts("FAIL: override bit set"); exit(1); }
    if (o[39] || o[40]) { puts("FAIL: test bytes set"); exit(1); }
    bool idle = o[2] & 0x08;
    (idle ? sentIdle : sentAnswer)++;
    if (idle || !storeWrites) return;
    for (int k = 0; k < 6; k++) { st.cfm[k] = scb::u16(o, 5 + 2 * k); st.rpm[k] = scb::u16(o, 17 + 2 * k); }
    st.percent = o[29]; st.blower = scb::u16(o, 30); st.sqft = scb::u16(o, 33); st.occupants = o[35]; st.runmin = o[36]; st.interval = o[37];
    st.bedrooms = o[38] & 0x3F; st.eac = o[38] & 0x40; st.autoMode = o[38] & 0x80; st.energySaver = o[2] & 2; st.powerOpen = o[2] & 4;
  }
};
static Sim *sim; static uint32_t clk;
static void sendFn(const uint8_t *f, size_t n) { sim->take(f, n, clk); }

static ScbSetJob::State run(ScbSetJob &job, ScbParser &p, Sim &s, uint32_t ms, uint32_t &t, const std::vector<uint8_t> *inject = nullptr, uint32_t injectAt = 0) {
  uint32_t end = t + ms;
  for (; (int32_t)(end - t) > 0; t += 850) {
    clk = t;
    if (inject && t >= injectAt) { p.feed(inject->data(), inject->size()); inject = nullptr; job.onFrame(t, p); }
    uint8_t f[51]; size_t n;
    s.frame(s.lastAnswer != 0xFFFFFFFF && t - s.lastAnswer < 9000, f, n);
    if (p.feed(f, n)) job.onFrame(t, p);
    job.tick(t, p);
    if (job.state == ScbSetJob::DONE || job.state == ScbSetJob::FAILED) break;
  }
  return job.state;
}
static void base(Sim &s) {
  for (int k = 0; k < 6; k++) { s.st.cfm[k] = 300 + 50 * k; s.st.rpm[k] = 1500; }
  s.st.percent = 20; s.st.blower = 300; s.st.sqft = 2000; s.st.occupants = 3; s.st.runmin = 20; s.st.interval = 4; s.st.bedrooms = 3; s.st.autoMode = true;
}
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (%s)\n", msg, job.result); return 1; } printf("ok: %s -> %s, idle %d answers %d\n", msg, job.result, s.sentIdle, s.sentAnswer); } while (0)

static int simulate() {
  {
    Sim s; base(s); sim = &s; ScbParser p; ScbSetJob job(sendFn); uint32_t t = 100000;
    job.clear(); job.add("cfm.Y2", "500"); job.add("air.eac", "on");
    clk = t; job.begin(t, true); run(job, p, s, 20000, t);
    CHECK(job.state == ScbSetJob::DONE && !strcmp(job.result, "stored") && s.st.cfm[2] == 500 && s.st.eac && s.st.cfm[0] == 300 && s.sentAnswer == 1, "write one airflow and EAC");
    int a = s.sentIdle + s.sentAnswer; run(job, p, s, 30000, t); CHECK(s.sentIdle + s.sentAnswer == a, "silent after the job");
  }
  {
    Sim s; base(s); sim = &s; ScbParser p; ScbSetJob job(sendFn); uint32_t t = 100000;
    job.clear(); clk = t; job.begin(t, true); run(job, p, s, 20000, t);
    CHECK(job.state == ScbSetJob::DONE && !strcmp(job.result, "read") && job.haveSettings && job.settings.cfm[1] == 350 && s.sentAnswer == 0, "read settings sends no answer");
  }
  {
    Sim s; base(s); sim = &s; ScbParser p; ScbSetJob job(sendFn); uint32_t t = 100000;
    job.clear(); job.add("cfm.G", "300"); clk = t; job.begin(t, true); run(job, p, s, 20000, t);
    CHECK(job.state == ScbSetJob::DONE && !strcmp(job.result, "no change") && s.sentAnswer == 0, "no change sends no answer");
  }
  {
    Sim s; base(s); sim = &s; ScbParser p; ScbSetJob job(sendFn); uint32_t t = 100000;
    job.clear(); job.add("cfm.Y2", "900"); clk = t; job.begin(t, true); run(job, p, s, 20000, t);
    CHECK(job.state == ScbSetJob::FAILED && s.sentAnswer == 0 && s.st.cfm[2] == 400, "900 CFM refused on a 2430");
  }
  {
    Sim s; base(s); s.storeWrites = false; sim = &s; ScbParser p; ScbSetJob job(sendFn); uint32_t t = 100000;
    job.clear(); job.add("cfm.Y2", "500"); clk = t; job.begin(t, true); run(job, p, s, 30000, t);
    CHECK(job.state == ScbSetJob::FAILED && s.sentAnswer == 3, "gives up after three answers");
  }
  {
    Sim s; base(s); sim = &s; ScbParser p; ScbSetJob job(sendFn); uint32_t t = 100000;
    uint8_t pc[43]; uint8_t f[51]; size_t n; s.frame(true, f, n); scb::answerFrame(f, s.st, pc);
    std::vector<uint8_t> v(pc, pc + 43);
    clk = t; p.feed(v.data(), v.size()); job.tick(t, p);
    job.clear(); job.add("cfm.Y2", "500"); bool started = job.begin(t + 1000, true);
    CHECK(!started && job.state == ScbSetJob::FAILED && s.st.cfm[2] == 400, "refused while another PC is answering");
    job.state = ScbSetJob::IDLE; started = job.begin(t + 11000, true); t += 11000; run(job, p, s, 20000, t);
    CHECK(started && !strcmp(job.result, "stored") && s.st.cfm[2] == 500, "allowed once that PC has been quiet 10 s");
  }
  {
    Sim s; base(s); sim = &s; ScbParser p; ScbSetJob job(sendFn);
    const char *bad[][2] = {{"test.flow", "5"}, {"override.G", "1"}, {"cfm.X", "300"}, {"cfm.G", "abc"}, {"air.sqft", "40000"}, {"damper", "open"}};
    for (auto &b : bad) if (job.add(b[0], b[1])) { printf("FAIL: accepted %s\n", b[0]); return 1; }
    uint32_t t = 0; CHECK(!job.begin(t, false) && job.state == ScbSetJob::FAILED, "bad keys refused, and no start without frames");
  }
  puts("PASS");
  return 0;
}

int main(int argc, char **argv) { return argc > 1 && !strcmp(argv[1], "sim") ? simulate() : frames(); }
