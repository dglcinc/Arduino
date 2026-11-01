// DS2482Bus.h: the Pico W carrier's 1-Wire bus on U2, a DS2482-100 at I2C 0x18 on GP4 and GP5, and the
// DS18B20 commands. The chip supplies the bus pull-up. The DS2482 sequences are those of
// AirHandler/OneWireBus.h, proven on the shield; the D2 fallback is gone, because this board has no such path.

#pragma once

#include <Wire.h>

class DS2482Bus {
 public:
  // True when the chip answered and passed its reset. False with no chip, or with VCC or GND open,
  // when it runs on the I2C pull-ups' leakage.
  bool begin() { ok = deviceReset(); return ok; }
  bool present() const { return ok; }

  uint8_t reset() {
    uint8_t s;
    if (!cmd(0xB4) || !waitIdle(s)) return 0;
    return (s & ST_PPD) && !(s & ST_SD) ? 1 : 0;
  }
  void write(uint8_t v) {
    uint8_t s;
    if (cmd(0xA5, v)) waitIdle(s);
  }
  uint8_t read() {
    uint8_t s;
    if (!cmd(0x96) || !waitIdle(s)) return 0xFF;
    if (!cmd(0xE1, 0xE1)) return 0xFF;           // read pointer to the data register
    int v = readByte();
    return v < 0 ? 0xFF : (uint8_t)v;
  }
  void select(const uint8_t rom[8]) {
    write(0x55);
    for (int i = 0; i < 8; i++) write(rom[i]);
  }

  void resetSearch() {
    lastDiscrepancy = 0;
    lastDevice = false;
    memset(romNo, 0, sizeof(romNo));
  }
  // The search through the DS2482's triplet command (Maxim AN3684). True while devices remain.
  bool search(uint8_t rom[8]) {
    if (lastDevice) return false;
    if (!reset()) { resetSearch(); return false; }
    write(0xF0);
    int idBit = 1, lastZero = 0, romByte = 0;
    uint8_t mask = 1;
    while (romByte < 8) {
      uint8_t dir;
      if (idBit < lastDiscrepancy) dir = (romNo[romByte] & mask) ? 1 : 0;
      else                         dir = idBit == lastDiscrepancy ? 1 : 0;
      uint8_t s;
      if (!cmd(0x78, dir ? 0x80 : 0x00) || !waitIdle(s)) { resetSearch(); return false; }
      bool sbr = s & ST_SBR, tsb = s & ST_TSB;
      dir = (s & ST_DIR) ? 1 : 0;
      if (sbr && tsb) { resetSearch(); return false; }   // nothing answered this bit
      if (!sbr && !tsb && !dir) lastZero = idBit;         // a discrepancy, 0 taken
      if (dir) romNo[romByte] |= mask; else romNo[romByte] &= ~mask;
      idBit++;
      mask <<= 1;
      if (!mask) { romByte++; mask = 1; }
    }
    if (crc8(romNo, 7) != romNo[7] || romNo[0] == 0) { resetSearch(); return false; }
    lastDiscrepancy = lastZero;
    if (lastDiscrepancy == 0) lastDevice = true;
    memcpy(rom, romNo, 8);
    return true;
  }

  // Starts a conversion on every probe; 750 ms at 12 bits. The probes are powered, so the bus stays free.
  bool convertAll() {
    if (!reset()) return false;
    write(0xCC);
    write(0x44);
    return true;
  }
  // NAN for a missing probe, a bad CRC or the 85 C power-on value.
  float readF(const uint8_t rom[8]) {
    uint8_t sp[9];
    if (!readScratchpad(rom, sp)) return NAN;
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    if (raw == 0x0550) return NAN;
    return raw / 16.0f * 9.0f / 5.0f + 32.0f;
  }
  // Sets a probe to 12 bits if it is not already. RAM only; the factory EEPROM value is 12 bits.
  void setResolution12(const uint8_t rom[8]) {
    uint8_t sp[9];
    if (!readScratchpad(rom, sp) || sp[4] == 0x7F) return;
    if (!reset()) return;
    select(rom);
    write(0x4E);
    write(sp[2]);
    write(sp[3]);
    write(0x7F);
  }

  static uint8_t crc8(const uint8_t *d, uint8_t len) {
    uint8_t c = 0;
    while (len--) {
      uint8_t b = *d++;
      for (uint8_t i = 0; i < 8; i++) {
        uint8_t mix = (c ^ b) & 1;
        c >>= 1;
        if (mix) c ^= 0x8C;
        b >>= 1;
      }
    }
    return c;
  }

 private:
  static const uint8_t ADDR = 0x18;
  static const uint8_t ST_1WB = 0x01, ST_PPD = 0x02, ST_SD = 0x04, ST_RST = 0x10,
                       ST_SBR = 0x20, ST_TSB = 0x40, ST_DIR = 0x80;
  bool    ok = false;
  uint8_t romNo[8] = {0};
  int     lastDiscrepancy = 0;
  bool    lastDevice = false;

  bool cmd(uint8_t c) {
    Wire.beginTransmission(ADDR);
    Wire.write(c);
    return Wire.endTransmission() == 0;
  }
  bool cmd(uint8_t c, uint8_t arg) {
    Wire.beginTransmission(ADDR);
    Wire.write(c);
    Wire.write(arg);
    return Wire.endTransmission() == 0;
  }
  int readByte() {
    if (Wire.requestFrom(ADDR, (uint8_t)1) != 1) return -1;
    return Wire.read();
  }
  // A reset takes about 1.2 ms and a byte about 0.6 ms, so 10 ms means the chip has stopped answering.
  bool waitIdle(uint8_t &s) {
    unsigned long start = millis();
    while (millis() - start < 10) {
      int v = readByte();
      if (v < 0) return false;
      s = (uint8_t)v;
      if (!(s & ST_1WB)) return true;
    }
    return false;
  }
  bool readScratchpad(const uint8_t rom[8], uint8_t sp[9]) {
    if (!reset()) return false;
    select(rom);
    write(0xBE);
    for (int i = 0; i < 9; i++) sp[i] = read();
    return crc8(sp, 8) == sp[8];
  }
  // Device reset, then active pull-up on, strong pull-up and overdrive off. The chip echoes the
  // configuration's low nibble when the write took.
  bool deviceReset() {
    if (!cmd(0xF0)) return false;
    delayMicroseconds(600);
    int s = readByte();
    if (s < 0 || !(s & ST_RST)) return false;
    if (!cmd(0xD2, 0xE1)) return false;
    return readByte() == 0x01;
  }
};
