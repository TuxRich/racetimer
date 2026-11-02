/* racetimer gate controller on a Teensy 4.1 with Ethernet: two Widscan58
 * receivers (top and bottom of one gate) on serial ports, pass validation,
 * and the web UI over Ethernet. GPL-3.0-or-later.
 *
 * The gate logic is gate/src's, the same as on the WROOM (shared_*.c).
 * Wiring (docs/teensy.md): each C5's TX to the Teensy's RX for its port and
 * back, common ground. */
#include <Arduino.h>
#include "esp_timer.h"
#include "teensy_gate.h"

#define C5_BAUD 2000000 /* Widscan58 env esp32c5_uart */
/* Top receiver: Serial1, RX1 pin 0, TX1 pin 1. */
#define TOP_PORT 1
#define TOP_RX 0
#define TOP_TX 1
/* Bottom receiver: Serial2, RX2 pin 7, TX2 pin 8. */
#define BOTTOM_PORT 2
#define BOTTOM_RX 7
#define BOTTOM_TX 8

/* micros() wraps every 71.6 minutes; loop() reads the clock far more often
 * than that, so each wrap is seen. */
int64_t esp_timer_get_time(void) {
    static uint32_t last;
    static uint64_t high;
    uint32_t now = micros();
    if (now < last) high += 1ull << 32;
    last = now;
    return (int64_t)(high | now);
}

void setup() {
    Serial.begin(115200); /* USB: the console */
    settings_init();
    c5_link_init(gate_levels, gate_line, gate_telemetry); /* before gate_init, which configures the link */
    gate_init();
    c5_link_start(FUSION_TOP, TOP_PORT, TOP_TX, TOP_RX, C5_BAUD);
    c5_link_start(FUSION_BOTTOM, BOTTOM_PORT, BOTTOM_TX, BOTTOM_RX, C5_BAUD);
    net_start();
    web_start();
}

void loop() {
    c5_link_poll();
    net_poll();
    web_poll();
}
