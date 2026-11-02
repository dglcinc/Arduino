// Host side of check_mqtt.py: MqttLite and MqttDiscovery over a real socket. Connects, publishes discovery and state, prints
// each message it receives, and on a "die" message drops the socket without saying goodbye, to prove the will.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "MqttLite.h"
#include "MqttDiscovery.h"

struct Sock {
  int fd = -1;
  size_t write(const uint8_t *p, size_t n) { return ::send(fd, p, n, 0); }
  int available() { int n = 0; ioctl(fd, FIONREAD, &n); return n; }
  int read() { uint8_t c; return ::recv(fd, &c, 1, 0) == 1 ? c : -1; }
  bool connected() { return fd >= 0; }
};
static bool die = false;
static mqttha::Unit U = {"4f2a", "Kid's \"Room\"", "AM-0001", "1.1.0", "kid-s-room"};
static void onMsg(const char *t, const char *p, bool r) {
  char key[20];
  bool ok = mqttha::commandKey(U, t, key, sizeof(key));
  printf("rx %s|%s|%d|%s\n", t, p, r, ok ? key : "-");
  fflush(stdout);
  if (!strcmp(p, "die")) die = true;
}
static uint32_t ms() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }

int main(int c, char **v) {
  Sock s;
  s.fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(atoi(v[1]));
  a.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(s.fd, (sockaddr *)&a, sizeof(a))) { perror("connect"); return 2; }
  const char *user = c > 2 ? v[2] : "", *pass = c > 3 ? v[3] : "";
  bool writes = c > 4 && !strcmp(v[4], "writes");
  MqttLite<Sock> m(s);
  m.connect("airmaster-4f2a", user, pass, "airmaster/4f2a/status", "offline", ms());
  uint32_t t0 = ms();
  while (!m.ready && !m.refused && ms() - t0 < 3000) m.poll(ms(), onMsg);
  printf("ready %d refused %d\n", m.ready, m.refused);
  fflush(stdout);
  if (!m.ready) return 1;
  m.publish("airmaster/4f2a/status", "online", true, ms());
  m.subscribe("airmaster/4f2a/set/#", ms());
  m.subscribe("homeassistant/status", ms());
  static char topic[100], body[900];
  for (size_t i = 0; mqttha::discovery(i, U, writes, 200, 750, topic, sizeof(topic), body, sizeof(body)); i++) {
    if (strlen(body) >= sizeof(body) - 1) { puts("TRUNCATED"); return 3; }
    m.publish(topic, body, true, ms());
  }
  // a long state message, as long as the firmware's line gets
  m.publish("airmaster/4f2a/state", "{\"wsup\":49.81,\"wret\":55.37,\"aret\":76.12,\"asup\":58.40,\"g\":1,\"y1\":1,\"y2\":0,\"cfm\":300,\"rpm\":1045,\"cfm_y1\":300,\"cal\":1,\"rssi\":-61}", false, ms());
  puts("published");
  fflush(stdout);
  t0 = ms();
  while (!die && ms() - t0 < 20000) {
    if (!m.poll(ms(), onMsg)) { puts("link dead"); return 4; }
    usleep(5000);
  }
  // no DISCONNECT: the broker must publish the will
  linger l{1, 0};
  setsockopt(s.fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
  close(s.fd);
  puts("dropped");
  return 0;
}
