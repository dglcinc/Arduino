// AirHandler_Bench — the zone sketch on the fabricated shield, for its final bench run on 24 VAC alone
// (pivac docs/air-handler-shield-assembly.md, Checks).
//
// All logic lives in AirHandler_impl.h, a symlink to ../AirHandler/AirHandler_impl.h. Copy
// arduino_secrets.h.example from the repo root to arduino_secrets.h in this folder and fill in the WiFi
// credentials before building. The ROMs are zero, so the node reads the first two probes on the bus in
// search order and scans the bus only at boot: plug the probes in before power-up.

#define ZONE_TAG       "BENCH"
#define HOSTNAME       "airhandler-bench"
#define SHIELD_PCB                         // return air on A0, supply on A1

#define ROM_WSUP       { 0, 0, 0, 0, 0, 0, 0, 0 }
#define ROM_WRET       { 0, 0, 0, 0, 0, 0, 0, 0 }

// NTC curve as AirHandler_MBR: beta fitted to the three bath points of 2026-10-20.
#define NTC_R0_OHM     10000.0f
#define NTC_BETA       3995.0f
#define NTC_RREF_OHM   10000.0f    // R8 and R9

#include "AirHandler_impl.h"
