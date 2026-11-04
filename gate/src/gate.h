/* The gate: both receivers' levels in, per-board pass detection (Widscan58's
 * lap.c), the fusion's verdicts out, and what the web UI shows.
 * GPL-3.0-or-later.
 *
 * Times: each C5 stamps its LEVELS frames with its own ms clock. The gate
 * puts them on its own clock (esp_timer) with a per-board offset, the
 * smallest (arrival - board time) over the last 1-2 s: the board's clock
 * plus the shortest link delay seen, good to about a millisecond. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "c5_link.h"
#include "gate_fusion.h"
#include "settings.h"

#define GATE_MAX_CH 8
#ifndef GATE_RACE_KEPT
#define GATE_RACE_KEPT 256 /* detections a race keeps for replay, the oldest dropped */
#endif

void gate_init(void);
/* New settings in use (settings_set has run): re-set up the boards if the
 * channels or gain changed, and apply thresholds and fusion limits. */
void gate_settings_changed(const settings_t *old, const settings_t *now);
/* c5_link callbacks. */
void gate_levels(unsigned board, const frame_levels_t *h, const frame_level_t *lv, int64_t rx_us);
void gate_line(unsigned board, const char *line);
void gate_telemetry(unsigned board, const frame_telemetry_t *t);
/* Average the next n paired passes into the top/bottom offset. */
void gate_calibrate(unsigned n);
/* Forget laps. */
void gate_clear(void);
/* The gate's clock, ms. */
uint32_t gate_now_ms(void);

/* JSON messages for the web UI. Each returns the length written (0: none). */
/* Each channel's highest level per board since the previous call. */
size_t gate_json_levels(char *buf, size_t cap);
/* The oldest queued event (a pass verdict, one board's pass, a reply line). */
size_t gate_json_event(char *buf, size_t cap);
/* Board health and calibration state. */
size_t gate_json_status(char *buf, size_t cap);
/* The settings in use and the channel names. */
size_t gate_json_config(char *buf, size_t cap);

/* For the timer API (gate_api.h). */

/* The gate's clock, microseconds since boot. */
int64_t gate_now_us(void);

/* A race: from its start, every verdict is kept as a detection, numbered
 * from 0. */
typedef struct {
    int64_t t_ms;   /* pass time: ms since the gate booted */
    uint16_t mhz;   /* the channel's frequency then, and its name */
    int16_t delta;  /* level codes (0.25 dB), offset applied; 0 for a single */
    uint8_t ch;
    uint8_t verdict; /* fusion_verdict_t */
    bool counted;    /* a lap by the gate's own rules (min lap) */
    uint8_t peak[FUSION_BOARDS]; /* top, bottom: level codes, 0 for the board that missed it */
    char name[6];
} gate_detection_t;

typedef struct {
    uint32_t no;      /* races so far: this one's number (0: none yet) */
    bool running;
    int64_t start_ms; /* the gate's clock */
    uint32_t count;   /* detections so far: the next one's number */
} gate_race_t;

/* Start a race: forget laps and any pass under way, keep detections from
 * now. start_ms: when it started, on the gate's clock. */
void gate_race_start(int64_t start_ms);
/* Stop keeping detections (those kept stay until the next start). */
void gate_race_stop(void);
/* The current or last race; whether it is running. */
bool gate_race(gate_race_t *out);
/* Detection seq of the current or last race, if still kept. */
bool gate_race_detection(uint32_t seq, gate_detection_t *out);
/* The channel plan in use: names and MHz; returns the count. */
unsigned gate_channels(char name[GATE_MAX_CH][8], uint16_t mhz[GATE_MAX_CH]);
/* The latest ~33 ms window's highest level per board and channel (what
 * gate_json_levels sent last), and whether either board is mid-crossing.
 * Returns the channel count. */
unsigned gate_levels_now(uint8_t level[C5_BOARDS][GATE_MAX_CH], bool crossing[GATE_MAX_CH]);

/* One receiver's health, as the page's status shows it. */
typedef struct {
    bool ok;          /* LEVELS frames in the last second */
    bool scanning;    /* the board says it is scanning */
    float hops_per_s; /* over the last status period */
    uint32_t crc_errors, bad_frames, setups, drops, clipped;
} gate_board_t;
void gate_board(unsigned board, gate_board_t *out);
