/*
 * Buzzer test for the rehab-hand PCB (Board1 / PCB1).
 *
 * Beeps the MLT-8530 magnetic buzzer (BUZZER1) in a loop. The MLT-8530 is a
 * magnetic transducer (NOT self-oscillating), so it must be driven with an AC
 * square wave near its 2.7 kHz resonant peak. ESP32 GPIO38 drives the low-side
 * N-FET (Q2, 2N7002) through a 470 ohm gate resistor; D-flyback is 1N4148.
 *
 * Standalone Arduino IDE sketch (pre-PlatformIO bring-up):
 *   Board:  "ESP32S3 Dev Module"  (install esp32 boards by Espressif)
 *   Upload: USB-C, native USB. Hold BOOT (SW1), tap RESET (SW2), release BOOT
 *           if it won't enter download mode on its own.
 *
 * No external library needed -- uses the esp32 core's tone().
 */

constexpr uint8_t BUZZER_PIN = 38;       // matches firmware/include/config.h
constexpr unsigned int BUZZER_FREQ = 2700; // MLT-8530 resonant peak (Hz)

constexpr unsigned long BEEP_MS = 200;   // tone on time
constexpr unsigned long GAP_MS  = 800;   // silence between beeps

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}

void loop() {
  tone(BUZZER_PIN, BUZZER_FREQ); // start square wave
  delay(BEEP_MS);
  noTone(BUZZER_PIN);            // stop
  digitalWrite(BUZZER_PIN, LOW); // ensure FET gate is low (buzzer off)
  delay(GAP_MS);
}
