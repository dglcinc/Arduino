/*
 * RS485Blast -- diagnostic only.  Saturates the RS-485 driver so a DC meter
 * across A/B can tell "the shield is driving" from "the shield is mute".
 *
 * Writes 0x55 (alternating bits) to Serial1 continuously.  No Modbus framing,
 * no function code, no valid CRC -- a listening slave cannot act on any of it.
 * The DFR0259 in AUTO mode asserts DE whenever data is flowing, so the driver
 * is held on essentially the whole time.
 *
 * Expected at the pair, with 680R bias fitted:
 *   idle (no transmit) ....... 0.44 V DC  (the bias divider alone)
 *   driving .................. anything else -- near 0 V DC, and volts on AC
 * A steady 0.44 V through this sketch means nothing leaves the shield.
 */

const uint32_t LINK_BAUD = 9600;

void setup() {
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD);
  pinMode(LED_BUILTIN, OUTPUT);
  delay(300);
  Serial.println(F("RS485Blast: writing 0x55 to Serial1 at 9600 8N1, continuously."));
  Serial.println(F("Meter on DC across A/B. 0.44 V steady = shield mute."));
}

void loop() {
  // write() blocks once the TX buffer fills, which keeps the UART saturated.
  for (int i = 0; i < 256; i++) Serial1.write(0x55);
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
}
