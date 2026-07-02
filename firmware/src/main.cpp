#include <Arduino.h>

#include "config.h"
#include "ble.h"
#include "drivers/servos.h"
#include "drivers/buzzer.h"
#include "drivers/audio.h"
#include "drivers/logging.h"
#include "drivers/sdcard.h"
#include "drivers/webfs.h"
#include "drivers/wifi_scan.h"

static void print_boot_banner() {
    Serial.printf("\n");
    Serial.printf("=========================================\n");
    Serial.printf("  %s v%s\n", cfg::FW_NAME, cfg::FW_VERSION);
    Serial.printf("  Built %s %s\n", __DATE__, __TIME__);
    Serial.printf("  Free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());
    Serial.printf("=========================================\n\n");
}

void setup() {
    Serial.begin(115200);
    // Wait briefly for the USB-CDC host to enumerate so the banner isn't lost.
    // Don't wait forever — the board must boot fine with no host attached.
    const uint32_t serial_wait_deadline = millis() + 1500;
    while (!Serial && millis() < serial_wait_deadline) {
        delay(10);
    }
    print_boot_banner();

    servos::init();
    buzzer::init();
    audio::init();
    sdcard::init();
    logging::init();
    wifi_scan::init();

    ble::init();

    Serial.println("[main] setup() complete; advertising BLE.");
}

void loop() {
    ble::loop();
    // Services the Wi-Fi HTTP upload server once it's been started (after a
    // STA connect). No-op otherwise.
    webfs::loop();
    // Drivers that need periodic servicing (e.g. timed buzzer auto-off,
    // audio sample feeding, gentle servo move-to-open ramps) tick themselves here.
    servos::tick();
    buzzer::tick();
    audio::tick();
    // Samples finger positions into the SD /logs CSV while an exercise runs.
    logging::tick();
    // While audio is streaming, don't sleep — audio::tick() already paces itself
    // by blocking in i2s_write, and a fixed delay here just steals refill time
    // and risks DMA underruns (audible glitches). Idle otherwise to yield CPU.
    if (!audio::is_playing()) delay(5);
}
