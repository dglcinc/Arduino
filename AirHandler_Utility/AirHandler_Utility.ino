// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// AirHandler_Utility — utility zone air-handler node on the rev A shield
// (pivac docs/utility-air-handler-node-plan.md).
//
// All logic lives in AirHandler_impl.h, a symlink to ../AirHandler/AirHandler_impl.h. This file holds
// only what differs between air handlers. Copy arduino_secrets.h.example from the repo root to
// arduino_secrets.h in this folder and fill in the WiFi credentials before building.
//
// The unit's calls, airflow and RPM come from its SCB over the Pi's USB (pivac.UnicoSCB_Utility), so
// TB1's Y2 and W2 positions are not wired and the node reports y2 0; pivac does not map it.

#define ZONE_TAG       "UTIL"
#define HOSTNAME       "airhandler-utility"
#define SHIELD_PCB                         // the fabricated shield: return air on A0, supply on A1

// DS18B20 ROMs for the coil's water supply and return: PA9A on the supply pipe and PA9B on the return,
// the closest pair of the 2026-11-01 batch (pivac docs/air-handler-probe-calibration-2026-11-01.md).
// w1 names 28-000000D47F08 and 28-000000D48567; the bytes below are the bus order.
#define ROM_WSUP       { 0x28, 0x08, 0x7F, 0xD4, 0x00, 0x00, 0x00, 0xB2 }   // PA9A
#define ROM_WRET       { 0x28, 0x67, 0x85, 0xD4, 0x00, 0x00, 0x00, 0x4E }   // PA9B

// NTC curve: the mean R25 and beta of NTC pair 1 (SUP 1 on A1, RET 1 on A0) from the three baths of
// 2026-11-01 (per-bead fits 9928/4004 and 9825/3999). Per-bead residuals are offsets in pivac config.
#define NTC_R0_OHM     9877.0f
#define NTC_BETA       4001.0f
#define NTC_RREF_OHM   10000.0f    // R1 and R2

#include "AirHandler_impl.h"
