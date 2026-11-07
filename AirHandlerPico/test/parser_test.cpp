// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// Runs ScbParser over a byte stream on a computer: read stdin, print one line per status frame.
// c++ -std=c++17 -I.. parser_test.cpp -o parser_test && python3 check_parser.py ./parser_test
#include <cstdio>
#include "ScbParser.h"
static void show(const ScbStatus &s) {
  printf("%d %d%d%d%d%d%d %d", s.flags, s.g, s.y1, s.y2, s.w1, s.w2, s.e, s.hasAirflow);
  for (int k = 0; k < 6; k++) printf(" %d", s.stored[k]);
  printf(" %d %d\n", s.actualCfm, s.actualRpm);
}
int main() {
  static uint8_t data[1 << 20];
  size_t n = fread(data, 1, sizeof(data), stdin);
  ScbParser p;
  p.onStatus = show;
  size_t chunk = 1;
  for (size_t i = 0; i < n; i += chunk) {
    chunk = (i % 7) + 1;                         // arbitrary USB packet sizes
    if (i + chunk > n) chunk = n - i;
    p.feed(data + i, chunk);
  }
  fprintf(stderr, "status %u log %u pc %u stray %u\n", p.nStatus, p.nLog, p.nPc, p.strayBytes);
}
