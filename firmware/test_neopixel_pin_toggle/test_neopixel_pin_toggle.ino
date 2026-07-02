/*
 * NeoPixel data-pin toggle test (level-shifter debug).
 *
 * Does NOT send WS2812B data. Just drives GPIO39 high/low at ~1 Hz so you can
 * probe the SN74LV1T34 (U3) with a multimeter / scope during bring-up:
 *
 *   U3 pin 2 (A, input)  should toggle 0 <-> 3.3 V   (GPIO39 side OK)
 *   U3 pin 4 (Y, output) should toggle 0 <-> ~5 V    (level shifter OK)
 *   U3 pin 5 (VCC)       should read steady ~5 V
 *   U3 pin 3 (GND)       should read 0 V
 *
 * If A toggles but Y does not -> U3 unpowered or bad solder joint.
 * If A does not toggle        -> GPIO39 / firmware / U1 side.
 *
 * (The LEDs may flicker/show garbage during this test - that's expected,
 *  this is a raw level toggle, not real pixel data.)
 *
 * Arduino IDE: Board = "ESP32S3 Dev Module". No libraries needed.
 */

constexpr uint8_t NEOPIXEL_PIN = 39;   // matches firmware/include/config.h
constexpr unsigned long TOGGLE_MS = 750;  // 0.5-1 s range

void setup() {
  pinMode(NEOPIXEL_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_PIN, LOW);
}

void loop() {
  digitalWrite(NEOPIXEL_PIN, HIGH);
  delay(TOGGLE_MS);
  digitalWrite(NEOPIXEL_PIN, LOW);
  delay(TOGGLE_MS);
}
