/*
 * NeoPixel blink test for the rehab-hand PCB (Board1 / PCB1).
 *
 * Blinks the 4 patient-facing WS2812B LEDs (LED2-LED5) on/off in a loop.
 * Data comes from ESP32-S3 GPIO39, level-shifted to 5V by U3 (SN74LV1T34).
 *
 * Standalone Arduino IDE sketch (pre-PlatformIO bring-up):
 *   Board:   "ESP32S3 Dev Module"  (install esp32 boards by Espressif)
 *   Library: "Adafruit NeoPixel"   (Library Manager)
 *   Upload:  USB-C, native USB (USB CDC On Boot can stay default).
 *            Hold BOOT (SW1), tap RESET (SW2), release BOOT if it won't enter
 *            download mode on its own.
 *
 * NOTE: these LEDs run off the 5V rail. On USB power alone the 5V rail is fed
 * via D8 from VBUS, so the LEDs light up without the barrel jack. (Only the
 * servo rail needs the DC jack.)
 */

#include <Adafruit_NeoPixel.h>

constexpr uint8_t NEOPIXEL_PIN   = 39;  // matches firmware/include/config.h
constexpr int     NEOPIXEL_COUNT = 4;   // LED2-LED5

constexpr uint8_t BRIGHTNESS = 40;      // 0-255, kept low for the bench
constexpr unsigned long BLINK_MS = 500; // on/off interval

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
}

void loop() {
  // All LEDs on (white).
  for (int i = 0; i < NEOPIXEL_COUNT; i++) {
    strip.setPixelColor(i, strip.Color(255, 255, 255));
  }
  strip.show();
  delay(BLINK_MS);

  // All LEDs off.
  strip.clear();
  strip.show();
  delay(BLINK_MS);
}
