// ArduinoPSI_Domestic — Fusch 200 PSI sensor, static IP 10.0.0.219
//
// All shared logic lives in ArduinoPSI_impl.h. The only things that differ
// between sketches are the two sensor constants defined below.
// See CLAUDE.md for hardware details and deployment instructions.

#define SENSOR_MAX_PSI 200.0f
#define SENSOR_MAX_V   5.0f

#include "ArduinoPSI_impl.h"
