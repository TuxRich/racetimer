/* c5_link.h on the Teensy's hardware serial ports. GPL-3.0-or-later.
 *
 * The same link as gate/src/c5_link.c on the WROOM, run from loop() instead
 * of a task per board: c5_link_poll() parses what each port has received
 * (c5_frame.c) and sends a setup one command per SETUP_GAP_US, so one
 * board's setup never holds up the other board's data. Each port has 16 KB
 * of receive buffer, ~180 ms of LEVELS frames at 2 Mbaud, for whatever
 * holds up loop() meanwhile. */
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "teensy_gate.h"

#define RX_MEMORY 16384
#define TX_MEMORY 512
#define READ_BUDGET 4096       /* bytes per board per poll */
#define SILENT_US 1500000      /* no LEVELS this long: set the board up again */
#define SETUP_EVERY_US 3000000 /* at most this often */
#define SETUP_GAP_US 30000     /* between setup commands */
#define SETUP_STEPS 10

typedef struct {
    HardwareSerialIMXRT *port;
    bool started;
    int baud, link_baud; /* baud != link_baud: a diagnostic speed, no setups */
    bool setup_now;
    int64_t last_setup_us;
    int setup_step; /* the next setup command; -1: no setup under way */
    int64_t setup_at_us;
    char setup_channels[sizeof("CHANNELS ") + 128];
    char setup_gain[24];
    c5_frame_parser_t parser;
    c5_stats_t stats;
} board_t;

static HardwareSerialIMXRT *const PORTS[] = {nullptr,  &Serial1, &Serial2, &Serial3, &Serial4,
                                             &Serial5, &Serial6, &Serial7, &Serial8};
static board_t boards[C5_BOARDS];
static DMAMEM uint8_t rx_memory[C5_BOARDS][RX_MEMORY];
static uint8_t tx_memory[C5_BOARDS][TX_MEMORY];
static c5_levels_cb_t on_levels;
static c5_line_cb_t on_line;
static c5_telemetry_cb_t on_telemetry;
static char cfg_channels[128] = "R1=5658 R3=5732 R6=5843 R8=5917";
static uint8_t cfg_gain = 45;

void c5_link_init(c5_levels_cb_t levels, c5_line_cb_t line, c5_telemetry_cb_t telemetry) {
    on_levels = levels;
    on_line = line;
    on_telemetry = telemetry;
}

void c5_link_configure(const char *channels, uint8_t gain) {
    snprintf(cfg_channels, sizeof(cfg_channels), "%s", channels);
    cfg_gain = gain;
}

void c5_link_send(unsigned board, const char *line) {
    if (board >= C5_BOARDS || !boards[board].started) return;
    boards[board].port->write((const uint8_t *)line, strlen(line));
    boards[board].port->write('\n');
}

void c5_link_set_baud(unsigned board, int baud) {
    if (board >= C5_BOARDS || !boards[board].started) return;
    board_t *b = &boards[board];
    if (baud <= 0) baud = b->link_baud;
    b->port->begin(baud); /* also empties the buffers */
    b->baud = baud;
    b->setup_step = -1;
    c5_frame_init(&b->parser);
}

void c5_link_setup(unsigned board) {
    if (board < C5_BOARDS) boards[board].setup_now = true;
}

void c5_link_stats(unsigned board, c5_stats_t *out) {
    if (board < C5_BOARDS) *out = boards[board].stats;
}

/* PROTOCOL MONITOR first: it resets the metrics to FULL. The capture and FFT
 * are Widscan58's RotorHazard-mode defaults (4 x 64-point FFT, 1.25 MHz
 * bins). The same commands as gate/src/c5_link.c sends. */
static void setup_next(unsigned board, int64_t now) {
    board_t *b = &boards[board];
    const char *const cmds[SETUP_STEPS] = {"PROTOCOL MONITOR", "SCAN STOP",    b->setup_channels,
                                           "MODE GROUP",       "FFT 64",       "CAPN 256",
                                           "SPEC 0 256 40000000 MAX",          "METRICS RSSI",
                                           b->setup_gain,      "SCAN START"};
    c5_link_send(board, cmds[b->setup_step++]);
    b->setup_at_us = now + SETUP_GAP_US;
    if (b->setup_step == SETUP_STEPS) {
        b->setup_step = -1;
        b->stats.setups++;
        b->last_setup_us = now;
    }
}

static void frame_done(unsigned board, int64_t now) {
    board_t *b = &boards[board];
    const frame_header_t *h = (const frame_header_t *)b->parser.frame;
    const uint8_t *p = b->parser.frame + sizeof(*h);
    b->stats.frames++;
    if (h->type == FRAME_LEVELS && h->len >= sizeof(frame_levels_t)) {
        const frame_levels_t *lh = (const frame_levels_t *)p;
        if (h->len < sizeof(*lh) + lh->nch * sizeof(frame_level_t)) {
            b->stats.bad_frames++;
            return;
        }
        b->stats.levels++;
        b->stats.last_levels_us = now;
        if (on_levels) on_levels(board, lh, (const frame_level_t *)(p + sizeof(*lh)), now);
    } else if (h->type == FRAME_TELEMETRY && h->len >= sizeof(frame_telemetry_t)) {
        if (on_telemetry) on_telemetry(board, (const frame_telemetry_t *)p);
    }
}

static void byte_in(unsigned board, uint8_t c, int64_t now) {
    board_t *b = &boards[board];
    switch (c5_frame_byte(&b->parser, c)) {
    case C5_LINE:
        if (on_line) on_line(board, b->parser.line);
        break;
    case C5_FRAME:
        frame_done(board, now);
        break;
    case C5_CRC_ERROR:
        b->stats.crc_errors++;
        break;
    case C5_BAD_FRAME:
        b->stats.bad_frames++;
        break;
    default:
        break;
    }
}

void c5_link_poll(void) {
    for (unsigned board = 0; board < C5_BOARDS; board++) {
        board_t *b = &boards[board];
        if (!b->started) continue;
        int64_t now = esp_timer_get_time();
        for (int n = 0; n < READ_BUDGET; n++) {
            int c = b->port->read();
            if (c < 0) break;
            byte_in(board, (uint8_t)c, now);
        }
        if (b->baud != b->link_baud) continue; /* listening at a diagnostic speed */
        bool silent = now - b->stats.last_levels_us > SILENT_US;
        if (b->setup_step < 0 && (b->setup_now || (silent && now - b->last_setup_us > SETUP_EVERY_US))) {
            b->setup_now = false;
            snprintf(b->setup_channels, sizeof(b->setup_channels), "CHANNELS %s", cfg_channels);
            snprintf(b->setup_gain, sizeof(b->setup_gain), "GAIN MANUAL %u", cfg_gain);
            b->setup_step = 0;
            b->setup_at_us = now;
        }
        if (b->setup_step >= 0 && now >= b->setup_at_us) setup_next(board, now);
    }
}

/* uart: the Teensy's port number (1 for Serial1 ... 8 for Serial8); the
 * pins are that port's (its usual ones, or an alternate it has). */
void c5_link_start(unsigned board, int uart, int tx_pin, int rx_pin, int baud) {
    if (board >= C5_BOARDS || uart < 1 || uart > 8) return;
    board_t *b = &boards[board];
    b->port = PORTS[uart];
    b->port->addMemoryForRead(rx_memory[board], RX_MEMORY);
    b->port->addMemoryForWrite(tx_memory[board], TX_MEMORY);
    b->port->setRX(rx_pin);
    b->port->setTX(tx_pin);
    b->port->begin(baud);
    b->baud = b->link_baud = baud;
    b->setup_step = -1;
    c5_frame_init(&b->parser);
    b->started = true;
    /* The board may still be booting: wait for its first silence check. */
    b->last_setup_us = esp_timer_get_time() - SETUP_EVERY_US + 500000;
}
