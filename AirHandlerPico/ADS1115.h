// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// ADS1115.h: U3 on the Pico W carrier, at I2C 0x48 with ADDR grounded. AIN0 and AIN1 are the supply and
// return NTC dividers and AIN2 is the 3.3 V rail that feeds them, so a reading is the ratio of the two
// and the rail's own value cancels. AIN3 is tied to 3.3 V and never read.
//
// One channel converts at a time at 128 samples/s, about 8 ms, and tick() never waits for it, so the
// loop keeps serving USB and HTTP. Each period() yields one ratio per divider, or NAN.

#pragma once

#include <Wire.h>

class ADS1115 {
 public:
  void tick() {
    uint32_t now = millis();
    if (!pending) {
      start(ch);
      return;
    }
    if (now - started < 10) return;
    pending = false;
    int16_t v;
    if (!readResult(v)) { bad = true; return; }
    if (v < 0) v = 0;
    sum[ch] += v;
    count[ch]++;
    ch = (ch + 1) % 3;
  }

  // Ends the averaging period. Returns false with no usable reading, as when the chip is absent or
  // the rail reads under 2.5 V.
  bool period(float &ratioSup, float &ratioRet) {
    bool good = !bad && count[0] && count[1] && count[2];
    float ref = good ? (float)sum[2] / count[2] : 0;
    if (good && ref * FS_V / 32768.0f < 2.5f) good = false;
    if (good) {
      ratioSup = ((float)sum[0] / count[0]) / ref;
      ratioRet = ((float)sum[1] / count[1]) / ref;
    }
    for (int k = 0; k < 3; k++) { sum[k] = 0; count[k] = 0; }
    bad = false;
    return good;
  }

 private:
  static constexpr float FS_V = 4.096f;
  static const uint8_t ADDR = 0x48;
  uint8_t  ch = 0;
  bool     pending = false, bad = false;
  uint32_t started = 0;
  uint32_t sum[3] = {0, 0, 0}, count[3] = {0, 0, 0};

  // Single shot, +/-4.096 V, 128 samples/s, comparator off. MUX 100, 101 and 110 are AIN0, 1 and 2 to GND.
  void start(uint8_t c) {
    static const uint8_t MSB[3] = {0xC3, 0xD3, 0xE3};
    Wire.beginTransmission(ADDR);
    Wire.write(0x01);
    Wire.write(MSB[c]);
    Wire.write(0x83);
    if (Wire.endTransmission() != 0) { bad = true; return; }
    started = millis();
    pending = true;
  }
  bool readResult(int16_t &v) {
    Wire.beginTransmission(ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom(ADDR, (uint8_t)2) != 2) return false;
    uint8_t hi = Wire.read(), lo = Wire.read();
    v = (int16_t)((hi << 8) | lo);
    return true;
  }
};
