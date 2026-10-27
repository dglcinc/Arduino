// OneWireBus.h — the air-handler nodes' 1-Wire bus, on a DS2482-100 or on D2, chosen at boot.
//
// The fabricated shield (pivac docs/air-handler-shield-pcb-plan.md §4.5) drives the bus from U2, a
// DS2482-100 at I2C 0x18 on the R4's SDA and SCL (A4, A5), which supplies the bus pull-up itself. JP1
// is its rollback: U2 off, JP1 bridged and R15 fitted put the bus on D2, as on the proto-shield. begin()
// looks for the DS2482 and falls back to D2 when nothing answers at 0x18, so one build serves both.
// Never fit U2 with JP1 bridged: D2 and the DS2482 would drive the same wire.
//
// DallasTemperature only accepts the OneWire class, so the DS18B20 commands are here too: convertAll()
// starts a conversion on every probe, and readF() reads one probe's scratchpad, checks its CRC and
// returns NAN for a missing probe, a bad CRC or the 85 C power-on value.

#pragma once

#include <Wire.h>
#include <OneWire.h>
#include "WDT.h"

class OneWireBus {
public:
  enum Master { NONE_D2, DS2482, DS2482_FAULT };

  OneWireBus(uint8_t pin, uint8_t i2cAddr = 0x18) : ow(pin), addr(i2cAddr) {}

  // Returns DS2482 when the chip answered and passed its reset, DS2482_FAULT when it answered but
  // failed (VCC or GND open: it then runs on the I2C pull-ups' leakage), NONE_D2 when nothing answered.
  Master begin() {
    Wire.begin();
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) {
      i2c = false;
      master = NONE_D2;
      return master;
    }
    i2c = true;
    master = dsmReset() ? DS2482 : DS2482_FAULT;
    return master;
  }

  const char *masterName() const {
    switch (master) {
      case DS2482:       return "DS2482 at 0x18";
      case DS2482_FAULT: return "DS2482 at 0x18 FAILED its reset: check 5 V between U2 pins 1 and 3";
      default:           return "D2 (no DS2482 at 0x18)";
    }
  }

  // --- the OneWire subset the sketches use --------------------------------------------------------

  // 1 when a device answered the reset with a presence pulse.
  uint8_t reset() {
    if (!i2c) return ow.reset();
    if (!cmd(0xB4)) return 0;
    uint8_t s;
    if (!waitIdle(s)) return 0;
    return (s & ST_PPD) && !(s & ST_SD) ? 1 : 0;
  }

  void write(uint8_t v) {
    if (!i2c) { ow.write(v); return; }
    uint8_t s;
    if (cmd(0xA5, v)) waitIdle(s);
  }

  uint8_t read() {
    if (!i2c) return ow.read();
    uint8_t s;
    if (!cmd(0x96) || !waitIdle(s)) return 0xFF;
    if (!cmd(0xE1, 0xE1)) return 0xFF;            // read pointer to the data register
    return readByte();
  }

  void skip() { write(0xCC); }

  void select(const uint8_t rom[8]) {
    write(0x55);
    for (int i = 0; i < 8; i++) write(rom[i]);
  }

  void reset_search() {
    if (!i2c) { ow.reset_search(); return; }
    lastDiscrepancy = 0;
    lastDevice = false;
    memset(romNo, 0, sizeof(romNo));
  }

  // The 1-Wire search on the DS2482 uses its triplet command (Maxim AN3684). True while devices remain.
  bool search(uint8_t rom[8]) {
    if (!i2c) return ow.search(rom);
    if (lastDevice) return false;
    if (!reset()) { reset_search(); return false; }
    write(0xF0);
    int idBit = 1, lastZero = 0, romByte = 0;
    uint8_t mask = 1;
    while (romByte < 8) {
      uint8_t dir;
      if (idBit < lastDiscrepancy) dir = (romNo[romByte] & mask) ? 1 : 0;
      else                         dir = idBit == lastDiscrepancy ? 1 : 0;
      uint8_t s;
      if (!cmd(0x78, dir ? 0x80 : 0x00) || !waitIdle(s)) { reset_search(); return false; }
      bool sbr = s & ST_SBR, tsb = s & ST_TSB;
      dir = (s & ST_DIR) ? 1 : 0;
      if (sbr && tsb) { reset_search(); return false; }   // nothing answered this bit
      if (!sbr && !tsb && !dir) lastZero = idBit;          // a discrepancy, 0 taken
      if (dir) romNo[romByte] |= mask; else romNo[romByte] &= ~mask;
      idBit++;
      mask <<= 1;
      if (!mask) { romByte++; mask = 1; }
    }
    if (OneWire::crc8(romNo, 7) != romNo[7] || romNo[0] == 0) { reset_search(); return false; }
    lastDiscrepancy = lastZero;
    if (lastDiscrepancy == 0) lastDevice = true;
    memcpy(rom, romNo, 8);
    return true;
  }

  // --- DS18B20 ------------------------------------------------------------------------------------

  // Starts a conversion on every probe; 750 ms at 12 bits. The probes are powered, so the bus is free
  // while they convert.
  bool convertAll() {
    if (!reset()) return false;
    skip();
    write(0x44);
    return true;
  }

  bool readScratchpad(const uint8_t rom[8], uint8_t sp[9]) {
    if (!reset()) return false;
    select(rom);
    write(0xBE);
    for (int i = 0; i < 9; i++) sp[i] = read();
    return OneWire::crc8(sp, 8) == sp[8];
  }

  float readF(const uint8_t rom[8]) {
    uint8_t sp[9];
    if (!readScratchpad(rom, sp)) return NAN;
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    if (raw == 0x0550) return NAN;                // 85.0 C: power-on value, no conversion yet
    return raw / 16.0f * 9.0f / 5.0f + 32.0f;
  }

  // Sets a probe to 12 bits if it is not already, keeping its alarm bytes. RAM only, as
  // DallasTemperature's setResolution does; the factory EEPROM value is 12 bits.
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

private:
  static const uint8_t ST_1WB = 0x01, ST_PPD = 0x02, ST_SD = 0x04, ST_RST = 0x10,
                       ST_SBR = 0x20, ST_TSB = 0x40, ST_DIR = 0x80;

  OneWire ow;
  uint8_t addr;
  bool    i2c = false;
  Master  master = NONE_D2;

  uint8_t romNo[8] = {0};
  int     lastDiscrepancy = 0;
  bool    lastDevice = false;

  bool cmd(uint8_t c) {
    Wire.beginTransmission(addr);
    Wire.write(c);
    return Wire.endTransmission() == 0;
  }

  bool cmd(uint8_t c, uint8_t arg) {
    Wire.beginTransmission(addr);
    Wire.write(c);
    Wire.write(arg);
    return Wire.endTransmission() == 0;
  }

  int readByte() {
    if (Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
    return Wire.read();
  }

  // Polls the status register until the 1-Wire line is idle. A reset takes about 1.2 ms and a byte
  // about 0.6 ms at standard speed, so 10 ms means the chip has stopped answering.
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

  // Device reset, then the configuration: active pull-up on, strong pull-up and overdrive off. The
  // chip echoes the configuration's low nibble when the write took.
  bool dsmReset() {
    if (!cmd(0xF0)) return false;
    delayMicroseconds(600);
    int s = readByte();
    if (s < 0 || !(s & ST_RST)) return false;
    if (!cmd(0xD2, 0xE1)) return false;           // APU = 1, upper nibble is its complement
    int c = readByte();
    return c == 0x01;
  }
};
