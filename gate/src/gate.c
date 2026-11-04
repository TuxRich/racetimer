/* GPL-3.0-or-later. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gate.h"
#include "gate_fusion.h"
#include "lap.h"

#define EVENTS 24
#define EVENT_BYTES 512
#define LEVEL_SATURATED (1u << 1) /* Widscan58's CHF_SATURATED (metrics.h) */
#define OFFSET_BUCKET_US 1000000

typedef struct {
    lap_seat_t seat[GATE_MAX_CH];
    uint8_t laps[GATE_MAX_CH];      /* seat lap count already reported */
    uint32_t last_t_ms[GATE_MAX_CH]; /* board time of the seat's previous update */
    uint8_t level_max[GATE_MAX_CH]; /* for the UI, since its last read */
    uint8_t last_max[GATE_MAX_CH];  /* what that read got (gate_levels_now) */
    /* Clock offset (gate us - board us): min over this bucket and the last */
    bool have_offset;
    int64_t offset_us, bucket_min, prev_min, bucket_start;
    /* Health */
    uint32_t saturated, telemetry_drops, link_fill_pm;
    bool running;
    uint32_t levels_at_status;
    float fps; /* LEVELS frames a second, over the last status period */
} board_state_t;

static SemaphoreHandle_t lock;
static board_state_t boards[C5_BOARDS];
static fusion_t fusion;
static char names[GATE_MAX_CH][8];
static uint16_t mhz[GATE_MAX_CH];
static unsigned nch;
/* The race (timer API): every verdict from its start, numbered from 0. */
static struct {
    uint32_t no; /* races so far: the current or last one's number */
    bool running;
    int64_t start_ms;
    uint32_t count; /* detections so far: the next one's number */
    gate_detection_t kept[GATE_RACE_KEPT]; /* detection n at n % GATE_RACE_KEPT */
} race;
static char events[EVENTS][EVENT_BYTES];
static unsigned ev_head, ev_n;
static int64_t status_at_us;
static bool cal_finished; /* a calibration's last pair came in: save its offset */

uint32_t gate_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

int64_t gate_now_us(void) { return esp_timer_get_time(); }

/* A recent time on the 32-bit ms clock (wraps every 49 days) in ms since boot. */
static int64_t ms_since_boot(uint32_t t_ms) {
    int64_t now = esp_timer_get_time() / 1000;
    return now - (uint32_t)((uint32_t)now - t_ms);
}

static void event_push(const char *json) {
    if (ev_n == EVENTS) {
        ev_head = (ev_head + 1) % EVENTS;
        ev_n--;
    }
    snprintf(events[(ev_head + ev_n) % EVENTS], EVENT_BYTES, "%s", json);
    ev_n++;
}

/* Channel names and MHz from the CHANNELS arguments ("R1=5658 R3=5732 ..."). */
static void parse_names(const char *channels) {
    nch = 0;
    const char *p = channels;
    while (*p && nch < GATE_MAX_CH) {
        while (*p == ' ') p++;
        const char *eq = strchr(p, '='), *end = strchr(p, ' ');
        if (!end) end = p + strlen(p);
        if (end == p) break;
        size_t len = (eq && eq < end ? eq : end) - p;
        if (len >= sizeof(names[0])) len = sizeof(names[0]) - 1;
        memcpy(names[nch], p, len);
        names[nch][len] = 0;
        mhz[nch] = eq && eq < end ? (uint16_t)strtoul(eq + 1, NULL, 10) : 0;
        nch++;
        p = end;
    }
}

static void fusion_cfg_from(const settings_t *s, fusion_cfg_t *c) {
    *c = (fusion_cfg_t){.pair_window_ms = s->pair_window_ms,
                        .pair_wait_ms = s->pair_wait_ms,
                        .delta_max = s->delta_max,
                        .offset = s->offset,
                        .min_lap_ms = s->min_lap_ms};
}

static void seats_init(const settings_t *s) {
    for (unsigned b = 0; b < C5_BOARDS; b++)
        for (unsigned ch = 0; ch < GATE_MAX_CH; ch++) {
            lap_init(&boards[b].seat[ch], s->enter_at, s->exit_at);
            boards[b].laps[ch] = 0;
        }
}

void gate_init(void) {
    lock = xSemaphoreCreateMutex();
    settings_t s;
    settings_get(&s);
    parse_names(s.channels);
    seats_init(&s);
    fusion_cfg_t c;
    fusion_cfg_from(&s, &c);
    fusion_init(&fusion, &c);
    c5_link_configure(s.channels, s.gain);
}

void gate_settings_changed(const settings_t *old, const settings_t *now) {
    bool resetup = strcmp(old->channels, now->channels) || old->gain != now->gain;
    xSemaphoreTake(lock, portMAX_DELAY);
    fusion_cfg_t c;
    fusion_cfg_from(now, &c);
    fusion_set_cfg(&fusion, &c);
    if (strcmp(old->channels, now->channels)) {
        parse_names(now->channels);
        fusion_clear(&fusion);
    }
    if (resetup) {
        seats_init(now); /* levels from before the change don't compare */
    } else if (old->enter_at != now->enter_at || old->exit_at != now->exit_at) {
        for (unsigned b = 0; b < C5_BOARDS; b++)
            for (unsigned ch = 0; ch < GATE_MAX_CH; ch++) {
                boards[b].seat[ch].enter_at = now->enter_at;
                boards[b].seat[ch].exit_at = now->exit_at;
            }
    }
    xSemaphoreGive(lock);
    if (resetup) {
        c5_link_configure(now->channels, now->gain);
        for (unsigned b = 0; b < C5_BOARDS; b++) c5_link_setup(b);
    }
}

static void clock_update(board_state_t *bs, uint32_t t_ms, int64_t rx_us) {
    int64_t sample = rx_us - (int64_t)t_ms * 1000;
    if (!bs->have_offset || rx_us - bs->bucket_start >= OFFSET_BUCKET_US) {
        bs->prev_min = bs->have_offset ? bs->bucket_min : sample;
        bs->bucket_min = sample;
        bs->bucket_start = rx_us;
        bs->have_offset = true;
    } else if (sample < bs->bucket_min) {
        bs->bucket_min = sample;
    }
    bs->offset_us = bs->bucket_min < bs->prev_min ? bs->bucket_min : bs->prev_min;
}

static const char *const VERDICT[] = {"VALID", "OVER", "UNDER", "SINGLE_TOP", "SINGLE_BOTTOM"};

/* In a race, a verdict is a detection. */
static void race_keep(const fusion_event_t *e) {
    gate_detection_t *d = &race.kept[race.count++ % GATE_RACE_KEPT];
    *d = (gate_detection_t){.t_ms = ms_since_boot(e->t_ms),
                            .mhz = e->ch < nch ? mhz[e->ch] : 0,
                            .delta = e->delta,
                            .ch = e->ch,
                            .verdict = e->verdict,
                            .counted = e->counted,
                            .peak = {e->peak[FUSION_TOP], e->peak[FUSION_BOTTOM]}};
    if (e->ch < nch) snprintf(d->name, sizeof(d->name), "%s", names[e->ch]);
}

static void drain_fusion(void) {
    fusion_event_t e;
    char json[256];
    while (fusion_next(&fusion, &e)) {
        snprintf(json, sizeof(json),
                 "{\"t\":\"pass\",\"ms\":%" PRIu32 ",\"ch\":%u,\"v\":\"%s\",\"n\":%d,\"cal\":%d,\"top\":%u,"
                 "\"bot\":%u,\"d\":%d,\"lap\":%" PRIu32 "}",
                 e.t_ms, e.ch, VERDICT[e.verdict], e.counted, e.calibration, e.peak[FUSION_TOP],
                 e.peak[FUSION_BOTTOM], e.delta, e.lap_ms);
        event_push(json);
        if (race.running) race_keep(&e);
    }
}

void gate_levels(unsigned board, const frame_levels_t *h, const frame_level_t *lv, int64_t rx_us) {
    if (board >= C5_BOARDS) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    board_state_t *bs = &boards[board];
    clock_update(bs, h->t_ms, rx_us);
    for (unsigned i = 0; i < h->nch; i++) {
        unsigned ch = lv[i].id;
        if (ch >= nch) continue;
        uint8_t level = lv[i].rssi < 1 ? 1 : lv[i].rssi > 254 ? 254 : lv[i].rssi;
        if (lv[i].flags & LEVEL_SATURATED) bs->saturated++;
        if (level > bs->level_max[ch]) bs->level_max[ch] = level;
        lap_seat_t *seat = &bs->seat[ch];
        uint32_t loop_us = bs->last_t_ms[ch] ? (h->t_ms - bs->last_t_ms[ch]) * 1000 : 0;
        bs->last_t_ms[ch] = h->t_ms;
        lap_feed(seat, h->t_ms, level, loop_us);
        if (seat->lap != bs->laps[ch]) {
            bs->laps[ch] = seat->lap;
            uint32_t t = (uint32_t)(((int64_t)seat->pass_ms * 1000 + bs->offset_us) / 1000);
            char json[96];
            snprintf(json, sizeof(json), "{\"t\":\"seat\",\"ms\":%" PRIu32 ",\"b\":%u,\"ch\":%u,\"pk\":%u}", t, board,
                     ch, seat->pass_peak);
            event_push(json);
            bool cal = fusion_calibrating(&fusion);
            fusion_pass(&fusion, board, ch, t, seat->pass_peak, (uint32_t)(rx_us / 1000));
            if (cal && !fusion_calibrating(&fusion)) cal_finished = true;
            drain_fusion();
        }
    }
    xSemaphoreGive(lock);
}

/* Reply lines go to the UI as they are, escaped for JSON. */
void gate_line(unsigned board, const char *line) {
    char json[EVENT_BYTES];
    size_t n = (size_t)snprintf(json, sizeof(json), "{\"t\":\"line\",\"b\":%u,\"s\":\"", board);
    for (const char *p = line; *p && n + 8 < sizeof(json); p++) {
        if (*p == '"' || *p == '\\') json[n++] = '\\';
        json[n++] = *p;
    }
    snprintf(json + n, sizeof(json) - n, "\"}");
    xSemaphoreTake(lock, portMAX_DELAY);
    event_push(json);
    xSemaphoreGive(lock);
}

void gate_telemetry(unsigned board, const frame_telemetry_t *t) {
    if (board >= C5_BOARDS) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    boards[board].telemetry_drops = t->drops;
    boards[board].link_fill_pm = t->link_fill_pm;
    boards[board].running = t->running;
    xSemaphoreGive(lock);
}

void gate_calibrate(unsigned n) {
    xSemaphoreTake(lock, portMAX_DELAY);
    fusion_calibrate(&fusion, n);
    xSemaphoreGive(lock);
}

void gate_clear(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    fusion_clear(&fusion);
    xSemaphoreGive(lock);
}

size_t gate_json_levels(char *buf, size_t cap) {
    xSemaphoreTake(lock, portMAX_DELAY);
    /* Singles wait for their pair; report them once the wait is over. Save
     * a calibration's result once it has completed (in gate_levels: only a
     * pair completes one). */
    fusion_poll(&fusion, gate_now_ms());
    drain_fusion();
    int16_t cal_offset = fusion.cfg.offset;
    bool cal_done = cal_finished;
    cal_finished = false;
    size_t n = (size_t)snprintf(buf, cap, "{\"t\":\"lv\",\"ms\":%" PRIu32 ",\"l\":[", gate_now_ms());
    for (unsigned b = 0; b < C5_BOARDS && n < cap; b++) {
        n += (size_t)snprintf(buf + n, cap - n, "%s[", b ? "," : "");
        for (unsigned ch = 0; ch < nch && n < cap; ch++) {
            n += (size_t)snprintf(buf + n, cap - n, "%s%u", ch ? "," : "", boards[b].level_max[ch]);
            boards[b].last_max[ch] = boards[b].level_max[ch];
            boards[b].level_max[ch] = 0;
        }
        if (n < cap) n += (size_t)snprintf(buf + n, cap - n, "]");
    }
    if (n < cap) n += (size_t)snprintf(buf + n, cap - n, "]}");
    xSemaphoreGive(lock);
    if (cal_done) {
        settings_t s;
        settings_get(&s);
        s.offset = cal_offset;
        settings_set(&s);
        settings_save();
    }
    return n < cap ? n : 0;
}

size_t gate_json_event(char *buf, size_t cap) {
    size_t n = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (ev_n) {
        n = (size_t)snprintf(buf, cap, "%s", events[ev_head]);
        ev_head = (ev_head + 1) % EVENTS;
        ev_n--;
    }
    xSemaphoreGive(lock);
    return n < cap ? n : 0;
}

size_t gate_json_status(char *buf, size_t cap) {
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    float secs = status_at_us ? (now - status_at_us) / 1e6f : 1;
    status_at_us = now;
    size_t n = (size_t)snprintf(buf, cap, "{\"t\":\"st\",\"up\":%" PRId64 ",\"cal\":%d,\"race\":%" PRIu32 ",\"b\":[",
                                now / 1000000, fusion_calibrating(&fusion), race.running ? race.no : 0);
    for (unsigned b = 0; b < C5_BOARDS && n < cap; b++) {
        c5_stats_t st;
        c5_link_stats(b, &st);
        board_state_t *bs = &boards[b];
        float fps = (st.levels - bs->levels_at_status) / secs;
        bs->levels_at_status = st.levels;
        bs->fps = fps;
        n += (size_t)snprintf(buf + n, cap - n,
                              "%s{\"ok\":%d,\"fps\":%.0f,\"crc\":%" PRIu32 ",\"bad\":%" PRIu32 ",\"setups\":%" PRIu32
                              ",\"drops\":%" PRIu32 ",\"fill\":%" PRIu32 ",\"sat\":%" PRIu32 ",\"run\":%d}",
                              b ? "," : "", st.last_levels_us && now - st.last_levels_us < 1000000, fps,
                              st.crc_errors, st.bad_frames, st.setups, bs->telemetry_drops, bs->link_fill_pm,
                              bs->saturated, bs->running);
    }
    if (n < cap) n += (size_t)snprintf(buf + n, cap - n, "]}");
    xSemaphoreGive(lock);
    return n < cap ? n : 0;
}

size_t gate_json_config(char *buf, size_t cap) {
    settings_t s;
    settings_get(&s);
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t n = (size_t)snprintf(buf, cap, "{\"t\":\"cfg\",\"names\":[");
    for (unsigned ch = 0; ch < nch && n < cap; ch++)
        n += (size_t)snprintf(buf + n, cap - n, "%s\"%s\"", ch ? "," : "", names[ch]);
    xSemaphoreGive(lock);
    if (n < cap)
        n += (size_t)snprintf(buf + n, cap - n,
                              "],\"channels\":\"%s\",\"gain\":%u,\"enter\":%u,\"exit\":%u,\"delta\":%u,\"offset\":%d,"
                              "\"pair\":%u,\"wait\":%u,\"minlap\":%" PRIu32 ",\"ssid\":\"%s\"}",
                              s.channels, s.gain, s.enter_at, s.exit_at, s.delta_max, s.offset, s.pair_window_ms,
                              s.pair_wait_ms, s.min_lap_ms, s.sta_ssid);
    return n < cap ? n : 0;
}

void gate_race_start(int64_t start_ms) {
    xSemaphoreTake(lock, portMAX_DELAY);
    fusion_clear(&fusion); /* laps count from the start; a pass under way is not this race's */
    race.no++;
    race.running = true;
    race.start_ms = start_ms;
    race.count = 0;
    xSemaphoreGive(lock);
}

void gate_race_stop(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    race.running = false;
    xSemaphoreGive(lock);
}

bool gate_race(gate_race_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = (gate_race_t){.no = race.no, .running = race.running, .start_ms = race.start_ms, .count = race.count};
    xSemaphoreGive(lock);
    return out->running;
}

bool gate_race_detection(uint32_t seq, gate_detection_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool kept = seq < race.count && race.count - seq <= GATE_RACE_KEPT;
    if (kept) *out = race.kept[seq % GATE_RACE_KEPT];
    xSemaphoreGive(lock);
    return kept;
}

unsigned gate_channels(char name[GATE_MAX_CH][8], uint16_t mhz_out[GATE_MAX_CH]) {
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned n = nch;
    memcpy(name, names, sizeof(names));
    memcpy(mhz_out, mhz, sizeof(mhz));
    xSemaphoreGive(lock);
    return n;
}

unsigned gate_levels_now(uint8_t level[C5_BOARDS][GATE_MAX_CH], bool crossing[GATE_MAX_CH]) {
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned n = nch;
    for (unsigned ch = 0; ch < n; ch++) {
        crossing[ch] = false;
        for (unsigned b = 0; b < C5_BOARDS; b++) {
            level[b][ch] = boards[b].last_max[ch];
            crossing[ch] |= boards[b].seat[ch].crossing;
        }
    }
    xSemaphoreGive(lock);
    return n;
}

void gate_board(unsigned board, gate_board_t *out) {
    *out = (gate_board_t){0};
    if (board >= C5_BOARDS) return;
    c5_stats_t st;
    c5_link_stats(board, &st);
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    const board_state_t *bs = &boards[board];
    *out = (gate_board_t){.ok = st.last_levels_us && now - st.last_levels_us < 1000000,
                          .scanning = bs->running,
                          .hops_per_s = bs->fps,
                          .crc_errors = st.crc_errors,
                          .bad_frames = st.bad_frames,
                          .setups = st.setups,
                          .drops = bs->telemetry_drops,
                          .clipped = bs->saturated};
    xSemaphoreGive(lock);
}
