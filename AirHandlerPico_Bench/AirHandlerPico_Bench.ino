// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright David Lewis
// AirHandlerPico_Bench: the Pico W carrier's zone sketch for the bench. All logic is in AirHandlerPico_impl.h, a
// symlink to ../AirHandlerPico. Copy arduino_secrets.h.example from the repo root to arduino_secrets.h in this folder
// and fill in the WiFi credentials before building. The ROMs are zero, so the node reads the first two probes on the
// bus in search order and logs every ROM it finds.

#define HOSTNAME       "airhandler-bench"
#define LOG_HOST       "10.0.0.84"

#define ROM_WSUP       { 0, 0, 0, 0, 0, 0, 0, 0 }
#define ROM_WRET       { 0, 0, 0, 0, 0, 0, 0, 0 }

// NTC curve as AirHandler_MBR. The NTCs need the three-bath calibration on this ADC.
#define NTC_R0_OHM     10000.0f
#define NTC_BETA       3995.0f
#define NTC_RREF_OHM   10000.0f    // R3 and R4

#include "AirHandlerPico_impl.h"
