// AirMaster: the product build. All logic is in AirHandlerPico_impl.h, a symlink to ../AirHandlerPico. It has no WiFi or
// passwords compiled in: AirMaster/factory writes each board's identity and the owner sets the rest from a browser.
// Build it with the 1 MB filesystem, which saved settings and firmware updates need:
//   arduino-cli compile -b rp2040:rp2040:rpipicow:usbstack=tinyusb_host,flash=2097152_1048576 --output-dir build AirMaster

// The NTC curve fitted to the three baths of 2026-10-20.
#define NTC_R0_OHM     10000.0f
#define NTC_BETA       3995.0f
#define NTC_RREF_OHM   10000.0f    // R3 and R4

#include "AirHandlerPico_impl.h"
