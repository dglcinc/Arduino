// AirHandler_MBR — master bedroom air-handler node (pivac docs/mbr-air-handler-node-plan.md).
//
// All logic lives in AirHandler_impl.h, a symlink to ../AirHandler/AirHandler_impl.h. This file holds
// only what differs between air handlers. Copy arduino_secrets.h.example from the repo root to
// arduino_secrets.h in this folder and fill in the WiFi credentials before building.

#define ZONE_TAG       "MBR"
#define HOSTNAME       "airhandler-mbr"

// DS18B20 ROMs for the coil's water supply and return, from the boot log (bring-up, plan §4.8).
// While both are zero the node reads the first two probes on the bus in search order.
#define ROM_WSUP       { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
#define ROM_WRET       { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }

// NTC curve: nominal resistance at 25 C and beta from the data sheet, checked in plan §6.
#define NTC_R0_OHM     10000.0f
#define NTC_BETA       3950.0f
#define NTC_RREF_OHM   10000.0f    // R8 and R9

#include "AirHandler_impl.h"
