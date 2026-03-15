// ArduinoPSI_BoilerLoop — Fusch 100 PSI sensor, static IP 10.0.0.114
//
// All shared logic lives in ArduinoPSI_impl.h. The only things that differ
// between sketches are the two sensor constants defined below.
// See CLAUDE.md for hardware details and deployment instructions.

#define SENSOR_MAX_PSI 100.0f
#define SENSOR_MAX_V   4.5f

#include "ArduinoPSI_impl.h"
