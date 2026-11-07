// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// MqttDiscovery.h: the Home Assistant MQTT discovery messages for one unit, with no Arduino calls. Each entity is one retained
// message under homeassistant/, read from the unit's one JSON state message. A reading the unit left out renders as None,
// which Home Assistant shows as unknown. The number entities exist only while the owner allows changes from Home Assistant.

#pragma once

#include <stdio.h>
#include <string.h>
#include "UnitCore.h"

namespace mqttha {

struct Entity {
  const char *component, *key, *name, *deviceClass, *unit, *category;
};

// component, key in the JSON line, name, device class, unit, entity category. The key is also the id.
const Entity ENTITIES[] = {
  {"sensor", "wsup", "Coil water supply", "temperature", "°F", ""},
  {"sensor", "wret", "Coil water return", "temperature", "°F", ""},
  {"sensor", "asup", "Supply air", "temperature", "°F", ""},
  {"sensor", "aret", "Return air", "temperature", "°F", ""},
  {"sensor", "cfm", "Airflow", "volume_flow_rate", "ft³/min", ""},
  {"sensor", "rpm", "Blower speed", "", "rpm", ""},
  {"sensor", "rssi", "WiFi signal", "signal_strength", "dBm", "diagnostic"},
  {"binary_sensor", "g", "Call G, fan", "running", "", ""},
  {"binary_sensor", "y1", "Call Y1, cool low", "running", "", ""},
  {"binary_sensor", "y2", "Call Y2, cool high", "running", "", ""},
  {"binary_sensor", "w1", "Call W1, heat low", "running", "", ""},
  {"binary_sensor", "w2", "Call W2, heat high", "running", "", ""},
  {"binary_sensor", "e", "Call E, emergency heat", "running", "", ""},
};
const size_t N_ENTITIES = sizeof(ENTITIES) / sizeof(ENTITIES[0]);

// The stored airflow per call: mode key as the SCB job names it, JSON key, name.
struct Stored { const char *mode, *key, *name; };
const Stored STORED[] = {
  {"G", "cfm_g", "Stored airflow, fan"},
  {"Y1", "cfm_y1", "Stored airflow, cool low"},
  {"Y2", "cfm_y2", "Stored airflow, cool high"},
  {"W1", "cfm_w1", "Stored airflow, heat low"},
  {"W2", "cfm_w2", "Stored airflow, heat high"},
  {"E", "cfm_e", "Stored airflow, emergency heat"},
};
const size_t N_STORED = 6;

struct Unit {
  const char *id;      // lower-case unit id, 4f2a
  const char *name;    // what the owner called it
  const char *serial, *fw, *host;
};

inline void baseTopic(const Unit &u, const char *leaf, char *out, size_t len) {
  snprintf(out, len, "airmaster/%s/%s", u.id, leaf);
}

inline void configTopic(const char *component, const Unit &u, const char *key, char *out, size_t len) {
  snprintf(out, len, "homeassistant/%s/airmaster_%s/%s/config", component, u.id, key);
}

inline size_t common(const Unit &u, const char *key, const char *name, char *b, size_t len) {
  char dn[80], nm[80];
  unit::jsonStr(dn, 0, sizeof(dn), u.name[0] ? u.name : u.id);
  unit::jsonStr(nm, 0, sizeof(nm), name);
  return snprintf(b, len,
    "{\"name\":\"%s\",\"unique_id\":\"airmaster_%s_%s\",\"state_topic\":\"airmaster/%s/state\",\"availability_topic\":\"airmaster/%s/status\","
    "\"device\":{\"identifiers\":[\"airmaster_%s\"],\"name\":\"%s\",\"manufacturer\":\"DL\",\"model\":\"AirMaster\",\"serial_number\":\"%s\",\"sw_version\":\"%s\","
    "\"configuration_url\":\"http://%s.local/\"},\"value_template\":\"{{ value_json.%s | default('None') }}\"",
    nm, u.id, key, u.id, u.id, u.id, dn, u.serial, u.fw, u.host, key);
}

// Writes entity i's topic and retained payload; false past the end. i runs through the sensors, then the stored airflows.
// With writes off, the airflow numbers come out as empty payloads, which removes them from Home Assistant.
inline bool discovery(size_t i, const Unit &u, bool writes, unsigned cfmMin, unsigned cfmMax, char *topic, size_t tlen, char *b, size_t len) {
  if (i < N_ENTITIES) {
    const Entity &e = ENTITIES[i];
    configTopic(e.component, u, e.key, topic, tlen);
    size_t n = common(u, e.key, e.name, b, len);
    if (e.deviceClass[0]) n += snprintf(b + n, len - n, ",\"device_class\":\"%s\"", e.deviceClass);
    if (e.unit[0]) n += snprintf(b + n, len - n, ",\"unit_of_measurement\":\"%s\",\"state_class\":\"measurement\"", e.unit);
    if (e.category[0]) n += snprintf(b + n, len - n, ",\"entity_category\":\"%s\"", e.category);
    if (!strcmp(e.component, "binary_sensor")) n += snprintf(b + n, len - n, ",\"payload_on\":\"1\",\"payload_off\":\"0\"");
    snprintf(b + n, len - n, "}");
    return true;
  }
  i -= N_ENTITIES;
  if (i >= N_STORED) return false;
  const Stored &s = STORED[i];
  configTopic("number", u, s.key, topic, tlen);
  if (!writes) {
    b[0] = 0;
    return true;
  }
  size_t n = common(u, s.key, s.name, b, len);
  snprintf(b + n, len - n,
           ",\"command_topic\":\"airmaster/%s/set/cfm/%s\",\"min\":%u,\"max\":%u,\"step\":25,\"mode\":\"box\",\"unit_of_measurement\":\"ft³/min\",\"device_class\":\"volume_flow_rate\",\"entity_category\":\"config\"}",
           u.id, s.mode, cfmMin, cfmMax);
  return true;
}

inline size_t discoveryCount() { return N_ENTITIES + N_STORED; }

// A command's topic to the SCB job's key: airmaster/4f2a/set/cfm/Y2 -> cfm.Y2. Only the stored airflows can be changed this way.
inline bool commandKey(const Unit &u, const char *topic, char *key, size_t len) {
  char prefix[48];
  snprintf(prefix, sizeof(prefix), "airmaster/%s/set/cfm/", u.id);
  size_t pl = strlen(prefix);
  if (strncmp(topic, prefix, pl)) return false;
  const char *mode = topic + pl;
  for (const Stored &s : STORED) {
    if (!strcmp(mode, s.mode)) {
      snprintf(key, len, "cfm.%s", s.mode);
      return true;
    }
  }
  return false;
}

}  // namespace mqttha
