// MqttLite.h: the small part of MQTT 3.1.1 the AirMaster needs, QoS 0 only, with no Arduino calls, so test/check_mqtt.py runs
// it against a real broker on the Mac. T supplies size_t write(const uint8_t *, size_t), int available(), int read() and
// bool connected(). A received packet too long for the buffer is read and dropped: commands here are short.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

template <class T>
class MqttLite {
 public:
  typedef void (*MessageFn)(const char *topic, const char *payload, bool retained);

  static const uint16_t KEEPALIVE_S = 60;
  static const uint32_t PING_MS = 30000, DEAD_MS = 95000;
  static const size_t   RX_MAX = 300;

  explicit MqttLite(T &t) : io(t) {}

  bool     ready = false;              // CONNACK accepted
  uint8_t  refused = 0;                // the broker's refusal code, 0 for none

  void connect(const char *clientId, const char *user, const char *pass, const char *willTopic, const char *willPayload, uint32_t now) {
    ready = false;
    refused = 0;
    rxState = 0;
    bool hasUser = user && *user, hasPass = hasUser && pass && *pass;
    size_t len = 10 + 2 + strlen(clientId) + 2 + strlen(willTopic) + 2 + strlen(willPayload);
    if (hasUser) len += 2 + strlen(user);
    if (hasPass) len += 2 + strlen(pass);
    header(0x10, len);
    static const uint8_t proto[6] = {0, 4, 'M', 'Q', 'T', 'T'};
    io.write(proto, 6);
    // protocol level 4; clean session, will retained at QoS 0, and the credentials present
    uint8_t v[4] = {4, (uint8_t)(0x02 | 0x04 | 0x20 | (hasUser ? 0x80 : 0) | (hasPass ? 0x40 : 0)), KEEPALIVE_S >> 8, KEEPALIVE_S & 0xFF};
    io.write(v, 4);
    str(clientId);
    str(willTopic);
    str(willPayload);
    if (hasUser) str(user);
    if (hasPass) str(pass);
    lastTx = lastRx = now;
  }

  void publish(const char *topic, const char *payload, bool retain, uint32_t now) {
    size_t tl = strlen(topic), pl = strlen(payload);
    header(0x30 | (retain ? 1 : 0), 2 + tl + pl);
    str(topic);
    io.write((const uint8_t *)payload, pl);
    lastTx = now;
  }

  void subscribe(const char *topic, uint32_t now) {
    size_t tl = strlen(topic);
    header(0x82, 2 + 2 + tl + 1);
    packetId = packetId == 0xFFFF ? 1 : packetId + 1;
    uint8_t id[2] = {(uint8_t)(packetId >> 8), (uint8_t)packetId};
    io.write(id, 2);
    str(topic);
    uint8_t qos = 0;
    io.write(&qos, 1);
    lastTx = now;
  }

  // Reads what has arrived and keeps the link alive. False when the broker has gone quiet past DEAD_MS or refused us.
  bool poll(uint32_t now, MessageFn fn) {
    int budget = 600;                  // bounded, so one flood cannot hold up the USB link
    while (budget-- > 0 && io.available() > 0) {
      int c = io.read();
      if (c < 0) break;
      feed((uint8_t)c, fn);
      lastRx = now;
    }
    if (refused) return false;
    if (now - lastRx > DEAD_MS) return false;
    if (now - lastTx > PING_MS) {
      static const uint8_t ping[2] = {0xC0, 0};
      io.write(ping, 2);
      lastTx = now;
    }
    return true;
  }

 private:
  T       &io;
  uint32_t lastTx = 0, lastRx = 0;
  uint16_t packetId = 0;
  int      rxState = 0;                // 0 type, 1 length, 2 body
  uint8_t  rxType = 0;
  size_t   rxLen = 0, rxGot = 0, rxMul = 1;
  uint8_t  rx[RX_MAX + 1];

  void header(uint8_t type, size_t len) {
    uint8_t h[5];
    size_t n = 0;
    h[n++] = type;
    do {
      uint8_t b = len % 128;
      len /= 128;
      if (len) b |= 0x80;
      h[n++] = b;
    } while (len && n < 5);
    io.write(h, n);
  }

  void str(const char *s) {
    size_t l = strlen(s);
    uint8_t h[2] = {(uint8_t)(l >> 8), (uint8_t)l};
    io.write(h, 2);
    io.write((const uint8_t *)s, l);
  }

  void feed(uint8_t c, MessageFn fn) {
    switch (rxState) {
      case 0:
        rxType = c;
        rxLen = 0;
        rxMul = 1;
        rxState = 1;
        break;
      case 1:
        rxLen += (c & 0x7F) * rxMul;
        rxMul *= 128;
        if (!(c & 0x80)) {
          rxGot = 0;
          if (rxLen == 0) { packet(fn); rxState = 0; }
          else rxState = 2;
        } else if (rxMul > 128UL * 128 * 128) {
          rxState = 0;                 // not a valid length
        }
        break;
      default:
        if (rxGot < RX_MAX) rx[rxGot] = c;
        rxGot++;
        if (rxGot == rxLen) { packet(fn); rxState = 0; }
        break;
    }
  }

  void packet(MessageFn fn) {
    uint8_t type = rxType >> 4;
    if (type == 2 && rxLen >= 2) {     // CONNACK
      if (rx[1] == 0) ready = true;
      else refused = rx[1];
    } else if (type == 3 && rxLen >= 2 && rxLen <= RX_MAX && !(rxType & 0x06)) {   // PUBLISH at QoS 0
      size_t tl = ((size_t)rx[0] << 8) | rx[1];
      if (tl + 2 > rxLen) return;
      static char topic[128];
      static char payload[RX_MAX];
      if (tl >= sizeof(topic)) return;
      memcpy(topic, rx + 2, tl);
      topic[tl] = 0;
      size_t pl = rxLen - 2 - tl;
      memcpy(payload, rx + 2 + tl, pl);
      payload[pl] = 0;
      if (fn) fn(topic, payload, rxType & 1);
    }
  }
};
