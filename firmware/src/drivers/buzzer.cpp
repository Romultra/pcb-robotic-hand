#include "buzzer.h"

#include "config.h"

namespace buzzer {

namespace {
constexpr uint8_t LEDC_CHANNEL = 7;        // servos use LEDC channels 0..4; stay clear
constexpr uint8_t LEDC_RESOLUTION_BITS = 10;
uint32_t g_stop_at_ms = 0;
bool     g_playing    = false;
}

void init() {
    // GPIO38 drives the gate of Q2 (2N7002) through R8 (470 Ω).
    // Coil sits between 3.3 V and Q2 drain; D6 (1N4148) is the flyback diode.
    pinMode(cfg::BUZZER_PIN, OUTPUT);
    digitalWrite(cfg::BUZZER_PIN, LOW);
    ledcSetup(LEDC_CHANNEL, 2700, LEDC_RESOLUTION_BITS);
    ledcAttachPin(cfg::BUZZER_PIN, LEDC_CHANNEL);
    ledcWrite(LEDC_CHANNEL, 0);
    Serial.printf("[buzzer] initialised on GPIO%u\n", cfg::BUZZER_PIN);
}

void play(uint16_t freq_hz, uint16_t duration_ms) {
    if (freq_hz == 0 || duration_ms == 0) {
        stop();
        return;
    }
    ledcWriteTone(LEDC_CHANNEL, freq_hz);
    ledcWrite(LEDC_CHANNEL, (1 << LEDC_RESOLUTION_BITS) / 2);  // 50% duty for square wave
    g_stop_at_ms = millis() + duration_ms;
    g_playing = true;
}

void stop() {
    ledcWriteTone(LEDC_CHANNEL, 0);
    ledcWrite(LEDC_CHANNEL, 0);
    digitalWrite(cfg::BUZZER_PIN, LOW);
    g_playing = false;
}

void tick() {
    if (g_playing && (int32_t)(millis() - g_stop_at_ms) >= 0) {
        stop();
    }
}

}  // namespace buzzer
