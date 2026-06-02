// ArduinoPSI_Domestic — Fusch 200 PSI sensor, IP 10.0.0.114 (DHW board; DHCP-reserved by MAC)
//
// All shared logic lives in ArduinoPSI_impl.h. The only things that differ
// between sketches are the two sensor constants defined below.
// See CLAUDE.md for hardware details and deployment instructions.

#define SENSOR_MAX_PSI 200.0f
#define SENSOR_MAX_V   5.0f

// DS18B20 on the DHW recirc loop, data line on D2. Defining ONE_WIRE_BUS here
// (and ONLY in this Domestic sketch) compile-enables the temperature code in
// the shared ArduinoPSI_impl.h. The BoilerLoop board leaves it undefined, so
// its {'psi' : ...} response and code stay completely unchanged. Requires the
// OneWire and DallasTemperature libraries at compile time.
#define ONE_WIRE_BUS   2

#include "ArduinoPSI_impl.h"
