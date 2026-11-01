// ScbParser.h: the Unico SCB's serial framing and status decode, ported from pivac's
// pivac/unico_scb_protocol.py (split_frames, decode_status). pivac docs/unico-scb-protocol.md is the
// specification. Plain C++ with no Arduino calls, so test/ can run it on a computer against the Python.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct ScbStatus {
  uint8_t  flags = 0;
  bool     g = false, y1 = false, y2 = false, w1 = false, w2 = false, e = false;
  bool     hasAirflow = false;          // false until a 36-, 37- or 51-byte frame has arrived
  uint16_t stored[6] = {0};             // CFM for G Y1 Y2 W1 W2 E
  uint16_t actualCfm = 0, actualRpm = 0;
};

class ScbParser {
 public:
  ScbStatus status;
  uint32_t  nStatus = 0, nLog = 0, nPc = 0, strayBytes = 0, overflows = 0;
  void    (*onStatus)(const ScbStatus &) = nullptr;   // called for every status frame

  // Feeds bytes in. Returns how many status frames they completed.
  int feed(const uint8_t *data, size_t len) {
    int frames = 0;
    while (len) {
      size_t room = sizeof(buf) - nBuf;
      if (!room) {                       // never expected at 4800 baud; drop the older half
        overflows++;
        memmove(buf, buf + sizeof(buf) / 2, nBuf - sizeof(buf) / 2);
        nBuf -= sizeof(buf) / 2;
        room = sizeof(buf) - nBuf;
      }
      size_t take = len < room ? len : room;
      memcpy(buf + nBuf, data, take);
      nBuf += take;
      data += take;
      len -= take;
      frames += parse();
    }
    return frames;
  }

  static bool fletcherOk(const uint8_t *f, size_t len) {
    uint16_t s1 = 0, s2 = 0;
    for (size_t i = 0; i < len; i++) {
      s1 = (s1 + f[i]) % 255;
      s2 = (s2 + s1) % 255;
    }
    return s1 == 0 && s2 == 0 && f[len - 1] != 0 && f[len - 2] != 0;
  }

 private:
  static const uint8_t STATUS_ID = 0x83, LOG_ID = 0x84, PC_TAG = 0x1C;

  uint8_t buf[1024];
  size_t  nBuf = 0;

  static uint16_t u16(const uint8_t *f, size_t i) { return (uint16_t(f[i]) << 8) | f[i + 1]; }

  void decode(const uint8_t *f, size_t len) {
    uint8_t calls = f[4] >> 2;           // active-low G Y1 Y2 W1 W2 E, bit 5..0
    status.flags = f[2];
    status.g  = !(calls & 0x20);
    status.y1 = !(calls & 0x10);
    status.y2 = !(calls & 0x08);
    status.w1 = !(calls & 0x04);
    status.w2 = !(calls & 0x02);
    status.e  = !(calls & 0x01);
    nStatus++;
    if (len == 9) {                      // calls and firmware only
      if (onStatus) onStatus(status);
      return;
    }
    size_t p = len == 37 ? 7 : 6;
    for (int k = 0; k < 6; k++) status.stored[k] = u16(f, p + 2 * k);
    status.actualCfm = u16(f, p + 24);
    status.actualRpm = u16(f, p + 26);
    status.hasAirflow = true;
    if (onStatus) onStatus(status);
  }

  int parse() {
    int frames = 0;
    size_t i = 0;
    while (i + 1 < nBuf) {
      if (buf[i] == 0x01 && (buf[i + 1] == STATUS_ID || buf[i + 1] == LOG_ID)) {
        size_t lens[3] = {0, 0, 0};
        int nl;
        bool haveFourth = i + 3 < nBuf;
        if (buf[i + 1] == LOG_ID) {
          lens[0] = 102; nl = 1;
        } else if (haveFourth && buf[i + 3] != 20 && buf[i + 3] != 23 && buf[i + 3] != PC_TAG) {
          lens[0] = 9; nl = 1;
        } else if (haveFourth && buf[i + 3] == PC_TAG) {
          lens[0] = 43; lens[1] = 31; nl = 2;
        } else {
          lens[0] = 36; lens[1] = 37; lens[2] = 51; nl = 3;
        }
        size_t hit = 0, longest = 0;
        for (int k = 0; k < nl; k++) {
          if (lens[k] > longest) longest = lens[k];
          if (!hit && i + lens[k] <= nBuf && fletcherOk(buf + i, lens[k])) hit = lens[k];
        }
        if (!hit && i + longest > nBuf) break;   // wait for more bytes
        if (hit) {
          if (buf[i + 1] == LOG_ID) nLog++;
          else if (buf[i + 3] == PC_TAG) nPc++;
          else { decode(buf + i, hit); frames++; }
          i += hit;
          continue;
        }
      }
      strayBytes++;
      i++;
    }
    memmove(buf, buf + i, nBuf - i);
    nBuf -= i;
    return frames;
  }
};
