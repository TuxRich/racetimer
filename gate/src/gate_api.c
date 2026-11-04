/* GPL-3.0-or-later. */
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gate.h"
#include "gate_api.h"
#include "settings.h"

#define LEVELS_MAX_HZ 30
#define STATUS_EVERY_MS 1000
#define START_WITHIN_MS 60000 /* a start further from the gate's now: the client's clock map is wrong */
#define MHZ_LO 5345           /* what the receivers tune (Widscan58's c5-race profile) */
#define MHZ_HI 5945
#define NAME_CHARS 5 /* Widscan58's channel names: 1 to 5 letters or digits */

static const char *const VERDICT[] = {"VALID", "OVER", "UNDER", "SINGLE_TOP", "SINGLE_BOTTOM"};
static const char *const BOARD[] = {"top", "bottom"};
static const char *const COMMANDS[] = {"hello", "time", "config", "frequencies", "start", "stop", "detections", "levels"};
static char gate_name[32] = "racegate", controller[16], version[48];

void gate_api_init(const char *name, const char *ctrl, const char *ver) {
    snprintf(gate_name, sizeof(gate_name), "%s", name);
    snprintf(controller, sizeof(controller), "%s", ctrl);
    snprintf(version, sizeof(version), "%s", ver);
}

/* ---- Reading requests: one JSON object. A key's value is found by skipping
 * the ones before it, so unknown keys, nested values and any order are fine. */

static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* Past the string starting at p (its opening quote), or NULL. */
static const char *skip_string(const char *p) {
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!*++p) return NULL; /* the escaped character is part of the string */
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Past the value at p, or NULL if there isn't one. */
static const char *skip_value(const char *p) {
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                if (!(p = skip_string(p))) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
            p++;
        }
        return NULL;
    }
    const char *start = p;
    while (*p && strchr("+-.0123456789eEtrufalsn", *p)) p++;
    return p > start ? p : NULL;
}

/* The value of a top-level key of the object, or NULL. */
static const char *find(const char *json, const char *key) {
    size_t key_len = strlen(key);
    const char *p = ws(json);
    if (*p++ != '{') return NULL;
    for (;;) {
        p = ws(p);
        if (*p != '"') return NULL;
        const char *k = p + 1, *end = skip_string(p);
        if (!end) return NULL;
        bool match = (size_t)(end - 1 - k) == key_len && !memcmp(k, key, key_len);
        p = ws(end);
        if (*p++ != ':') return NULL;
        p = ws(p);
        if (match) return p;
        if (!(p = skip_value(p))) return NULL;
        p = ws(p);
        if (*p++ != ',') return NULL; /* the end: not there */
    }
}

/* The string at p into out (cut short at cap); past it, or NULL. */
static const char *read_string(const char *p, char *out, size_t cap) {
    size_t n = 0;
    if (*p != '"') return NULL;
    for (p++; *p != '"'; p++) {
        char c = *p;
        if (!c) return NULL;
        if (c == '\\') {
            c = *++p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c == 'u') { /* ASCII as itself, the rest as '?' */
                char hex[5] = {0}, *end;
                for (int i = 0; i < 4; i++)
                    if (!(hex[i] = p[1 + i])) return NULL;
                unsigned long v = strtoul(hex, &end, 16);
                if (*end) return NULL;
                c = v < 0x80 ? (char)v : '?';
                p += 4;
            } else if (!c) {
                return NULL;
            }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return p + 1;
}

static bool get_number(const char *json, const char *key, double *out) {
    const char *p = find(json, key);
    char *end;
    if (!p || *p == '"') return false;
    *out = strtod(p, &end);
    return end != p;
}

static bool get_string(const char *json, const char *key, char *out, size_t cap) {
    const char *p = find(json, key);
    return p && read_string(p, out, cap);
}

/* An array of numbers: up to max into out; their count, or -1. */
static int get_numbers(const char *json, const char *key, double *out, int max) {
    const char *p = find(json, key);
    int n = 0;
    if (!p || *p != '[') return -1;
    p = ws(p + 1);
    if (*p == ']') return 0;
    for (;;) {
        char *end;
        if (n == max || *p == '"') return -1;
        out[n++] = strtod(p, &end);
        if (end == p) return -1;
        p = ws(end);
        if (*p == ']') return n;
        if (*p++ != ',') return -1;
        p = ws(p);
    }
}

/* An array of strings, each cut short at 8 bytes: their count, or -1. */
static int get_strings(const char *json, const char *key, char out[][8], int max) {
    const char *p = find(json, key);
    int n = 0;
    if (!p || *p != '[') return -1;
    p = ws(p + 1);
    if (*p == ']') return 0;
    for (;;) {
        if (n == max || !(p = read_string(p, out[n++], 8))) return -1;
        p = ws(p);
        if (*p == ']') return n;
        if (*p++ != ',') return -1;
        p = ws(p);
    }
}

/* ---- Writing messages. A message that doesn't fit isn't sent. */

typedef struct {
    char *buf;
    size_t cap, n;
} out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *o, const char *fmt, ...) {
    if (o->n >= o->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o->buf + o->n, o->cap - o->n, fmt, ap);
    va_end(ap);
    o->n += k > 0 ? (size_t)k : 0;
}

static void put_string(out_t *o, const char *s) {
    put(o, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') put(o, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) put(o, "\\u%04x", *s);
        else put(o, "%c", *s);
    }
    put(o, "\"");
}

static size_t done(const out_t *o) { return o->n < o->cap ? o->n : 0; }

static const char *flag(bool b) { return b ? "true" : "false"; }

/* A request's own fields, which its reply repeats. */
typedef struct {
    const char *cmd; /* "": not one we know */
    bool has_id;
    long long id;
} req_t;

/* {"type":..., then the request's cmd and id */
static void head(out_t *o, const char *type, const req_t *r) {
    put(o, "{\"type\":\"%s\"", type);
    if (r->cmd[0]) put(o, ",\"cmd\":\"%s\"", r->cmd);
    if (r->has_id) put(o, ",\"id\":%lld", r->id);
}

/* Returns false, for "return fail(...)" in a check. */
static bool fail(out_t *o, const req_t *r, const char *why) {
    head(o, "error", r);
    put(o, ",\"error\":");
    put_string(o, why);
    put(o, "}");
    return false;
}

static void hello(out_t *o, const req_t *r) {
    head(o, "hello", r);
    put(o, ",\"api\":%d,\"name\":", GATE_API_VERSION);
    put_string(o, gate_name);
    put(o, ",\"controller\":");
    put_string(o, controller);
    put(o, ",\"version\":");
    put_string(o, version);
    put(o, ",\"boards\":%d,\"max_channels\":%d,\"time_ms\":%.3f}", C5_BOARDS, GATE_MAX_CH, gate_now_us() / 1000.0);
}

static void config(out_t *o, const req_t *r) {
    settings_t s;
    char names[GATE_MAX_CH][8];
    uint16_t mhz[GATE_MAX_CH];
    settings_get(&s);
    unsigned n = gate_channels(names, mhz);
    head(o, "config", r);
    put(o, ",\"mhz\":[");
    for (unsigned i = 0; i < n; i++) put(o, "%s%u", i ? "," : "", mhz[i]);
    put(o, "],\"names\":[");
    for (unsigned i = 0; i < n; i++) {
        if (i) put(o, ",");
        put_string(o, names[i]);
    }
    put(o,
        "],\"enter\":%u,\"exit\":%u,\"gain\":%u,\"delta_db\":%.2f,\"offset_db\":%.2f,\"pair_ms\":%u,\"wait_ms\":%u,"
        "\"min_lap_ms\":%" PRIu32 "}",
        s.enter_at, s.exit_at, s.gain, s.delta_max / 4.0, s.offset / 4.0, s.pair_window_ms, s.pair_wait_ms,
        s.min_lap_ms);
}

static bool valid_name(const char *s) {
    size_t n = strlen(s);
    for (const char *p = s; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return false;
    return n >= 1 && n <= NAME_CHARS;
}

/* The channel plan: what the receivers scan and detections are on. Used at
 * once, not saved; a new plan restarts the receivers' scan (~0.6 s). */
static bool frequencies(out_t *o, const req_t *r, const char *json) {
    double f[GATE_MAX_CH + 1];
    char names[GATE_MAX_CH + 1][8];
    int n = get_numbers(json, "mhz", f, GATE_MAX_CH + 1);
    if (n < 1 || n > GATE_MAX_CH) return fail(o, r, "mhz: 1 to 8 frequencies");
    for (int i = 0; i < n; i++) {
        if (f[i] < MHZ_LO || f[i] > MHZ_HI || f[i] != (int)f[i]) return fail(o, r, "mhz: 5345 to 5945, whole MHz");
        for (int j = 0; j < i; j++)
            if (f[j] == f[i]) return fail(o, r, "mhz: the same frequency twice");
    }
    if (find(json, "names")) {
        if (get_strings(json, "names", names, GATE_MAX_CH + 1) != n) return fail(o, r, "names: one per frequency");
        for (int i = 0; i < n; i++)
            if (!valid_name(names[i])) return fail(o, r, "names: 1 to 5 letters or digits each");
    } else {
        for (int i = 0; i < n; i++) snprintf(names[i], sizeof(names[i]), "%d", (int)f[i]);
    }
    settings_t old, s;
    settings_get(&old);
    s = old;
    size_t len = 0;
    for (int i = 0; i < n; i++)
        len += (size_t)snprintf(s.channels + len, sizeof(s.channels) - len, "%s%s=%d", i ? " " : "", names[i], (int)f[i]);
    bool changed = strcmp(old.channels, s.channels) != 0;
    if (changed) {
        settings_set(&s);
        gate_settings_changed(&old, &s);
    }
    head(o, "ok", r);
    put(o, ",\"changed\":%s}", flag(changed));
    return true;
}

static bool boards_ok(void) {
    for (unsigned b = 0; b < C5_BOARDS; b++) {
        gate_board_t g;
        gate_board(b, &g);
        if (!g.ok) return false;
    }
    return true;
}

static bool start(out_t *o, gate_api_client_t *c, const req_t *r, const char *json) {
    int64_t now = gate_now_us() / 1000, at = now;
    double v;
    if (find(json, "start_ms")) {
        if (!get_number(json, "start_ms", &v)) return fail(o, r, "start_ms: a number");
        if (v < (double)(now - START_WITHIN_MS) || v > (double)(now + START_WITHIN_MS))
            return fail(o, r, "start_ms: more than a minute from the gate's clock (sync with time first)");
        at = (int64_t)(v < 0 ? v - 0.5 : v + 0.5);
    }
    gate_race_start(at);
    gate_race_t race;
    gate_race(&race);
    c->race_no = race.no;
    c->next_seq = 0;
    char line[48];
    snprintf(line, sizeof(line), "OK gate: race %" PRIu32 " started (timer API)", race.no);
    gate_line(C5_BOARDS, line);
    head(o, "ok", r);
    put(o, ",\"race\":%" PRIu32 ",\"start_ms\":%" PRId64 ",\"ready\":%s}", race.no, at, flag(boards_ok()));
    return true;
}

static void stop(out_t *o, const req_t *r) {
    gate_race_t race;
    bool was = gate_race(&race);
    gate_race_stop();
    if (was) {
        char line[64];
        snprintf(line, sizeof(line), "OK gate: race %" PRIu32 " stopped, %" PRIu32 " detections", race.no, race.count);
        gate_line(C5_BOARDS, line);
    }
    head(o, "ok", r);
    put(o, ",\"race\":%" PRIu32 ",\"detections\":%" PRIu32 "}", race.no, race.count);
}

/* Send the race's detections again, from one on. */
static bool replay(out_t *o, gate_api_client_t *c, const req_t *r, const char *json) {
    double v;
    if (!get_number(json, "from", &v) || v < 0 || v > 4294967295.0 || v != (uint32_t)v)
        return fail(o, r, "from: a detection number");
    gate_race_t race;
    gate_race(&race);
    uint32_t oldest = race.count > GATE_RACE_KEPT ? race.count - GATE_RACE_KEPT : 0;
    c->race_no = race.no;
    c->next_seq = (uint32_t)v < oldest ? oldest : (uint32_t)v;
    head(o, "ok", r);
    put(o, ",\"race\":%" PRIu32 ",\"detections\":%" PRIu32 ",\"from\":%" PRIu32 "}", race.no, race.count,
        c->next_seq);
    return true;
}

static bool levels_rate(out_t *o, gate_api_client_t *c, const req_t *r, const char *json) {
    double v;
    if (!get_number(json, "hz", &v) || v < 0 || v > LEVELS_MAX_HZ || v != (int)v)
        return fail(o, r, "hz: 0 (off) to 30");
    c->levels_hz = (uint16_t)v;
    head(o, "ok", r);
    put(o, ",\"hz\":%u}", c->levels_hz);
    return true;
}

size_t gate_api_hello(gate_api_client_t *c, char *buf, size_t cap) {
    *c = (gate_api_client_t){0};
    gate_race_t race;
    gate_race(&race);
    c->race_no = race.no;
    c->next_seq = race.count; /* only new detections, unless asked (replay) */
    out_t o = {buf, cap, 0};
    hello(&o, &(req_t){.cmd = ""});
    return done(&o);
}

size_t gate_api_request(gate_api_client_t *c, const char *json, char *buf, size_t cap) {
    out_t o = {buf, cap, 0};
    req_t r = {.cmd = ""};
    char cmd[16];
    double id;
    if (get_number(json, "id", &id) && id >= -9e15 && id <= 9e15 && id == (double)(long long)id) {
        r.has_id = true;
        r.id = (long long)id;
    }
    if (!find(json, "cmd")) {
        fail(&o, &r, ws(json)[0] == '{' ? "no cmd" : "not a JSON object");
        return done(&o);
    }
    if (get_string(json, "cmd", cmd, sizeof(cmd)))
        for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++)
            if (!strcmp(cmd, COMMANDS[i])) r.cmd = COMMANDS[i];
    if (!r.cmd[0]) fail(&o, &r, "unknown cmd");
    else if (!strcmp(r.cmd, "hello")) hello(&o, &r);
    else if (!strcmp(r.cmd, "time")) {
        head(&o, "time", &r);
        put(&o, ",\"time_ms\":%.3f}", gate_now_us() / 1000.0);
    } else if (!strcmp(r.cmd, "config")) config(&o, &r);
    else if (!strcmp(r.cmd, "frequencies")) frequencies(&o, &r, json);
    else if (!strcmp(r.cmd, "start")) start(&o, c, &r, json);
    else if (!strcmp(r.cmd, "stop")) stop(&o, &r);
    else if (!strcmp(r.cmd, "detections")) replay(&o, c, &r, json);
    else levels_rate(&o, c, &r, json);
    return done(&o);
}

static void detection(out_t *o, const gate_race_t *race, uint32_t seq, const gate_detection_t *d) {
    uint8_t top = d->peak[FUSION_TOP], bottom = d->peak[FUSION_BOTTOM];
    bool single = d->verdict == FUSION_SINGLE_TOP || d->verdict == FUSION_SINGLE_BOTTOM;
    put(o, "{\"type\":\"detection\",\"race\":%" PRIu32 ",\"seq\":%" PRIu32 ",\"mhz\":%u,\"name\":", race->no, seq,
        d->mhz);
    put_string(o, d->name);
    put(o,
        ",\"time_ms\":%" PRId64 ",\"race_ms\":%" PRId64 ",\"verdict\":\"%s\",\"valid\":%s,\"counted\":%s,"
        "\"peak\":%u,\"top\":%u,\"bottom\":%u,\"delta_db\":",
        d->t_ms, d->t_ms - race->start_ms, VERDICT[d->verdict], flag(d->verdict == FUSION_VALID), flag(d->counted),
        top > bottom ? top : bottom, top, bottom);
    if (single) put(o, "null}");
    else put(o, "%.2f}", d->delta / 4.0);
}

static void levels(out_t *o) {
    uint8_t lv[C5_BOARDS][GATE_MAX_CH];
    bool crossing[GATE_MAX_CH];
    char names[GATE_MAX_CH][8];
    uint16_t mhz[GATE_MAX_CH];
    unsigned n = gate_levels_now(lv, crossing), plan = gate_channels(names, mhz);
    if (plan < n) n = plan; /* the plan changed in between */
    put(o, "{\"type\":\"levels\",\"time_ms\":%" PRId64 ",\"mhz\":[", gate_now_us() / 1000);
    for (unsigned i = 0; i < n; i++) put(o, "%s%u", i ? "," : "", mhz[i]);
    for (unsigned b = 0; b < C5_BOARDS; b++) {
        put(o, "],\"%s\":[", BOARD[b]);
        for (unsigned i = 0; i < n; i++) put(o, "%s%u", i ? "," : "", lv[b][i]);
    }
    put(o, "],\"crossing\":[");
    for (unsigned i = 0; i < n; i++) put(o, "%s%s", i ? "," : "", flag(crossing[i]));
    put(o, "]}");
}

static void status(out_t *o, const gate_race_t *race) {
    put(o,
        "{\"type\":\"status\",\"time_ms\":%" PRId64 ",\"race\":{\"no\":%" PRIu32 ",\"running\":%s,\"start_ms\":%" PRId64
        ",\"detections\":%" PRIu32 "},\"boards\":[",
        gate_now_us() / 1000, race->no, flag(race->running), race->start_ms, race->count);
    for (unsigned b = 0; b < C5_BOARDS; b++) {
        gate_board_t g;
        gate_board(b, &g);
        put(o,
            "%s{\"name\":\"%s\",\"ok\":%s,\"scanning\":%s,\"hops_per_s\":%.0f,\"crc_errors\":%" PRIu32
            ",\"bad_frames\":%" PRIu32 ",\"setups\":%" PRIu32 ",\"drops\":%" PRIu32 ",\"clipped\":%" PRIu32 "}",
            b ? "," : "", BOARD[b], flag(g.ok), flag(g.scanning), g.hops_per_s, g.crc_errors, g.bad_frames, g.setups,
            g.drops, g.clipped);
    }
    put(o, "]}");
}

size_t gate_api_next(gate_api_client_t *c, uint32_t now_ms, char *buf, size_t cap) {
    out_t o = {buf, cap, 0};
    gate_race_t race;
    gate_race(&race);
    if (c->race_no != race.no) { /* a new race: its detections from the first */
        c->race_no = race.no;
        c->next_seq = 0;
    }
    if (c->next_seq < race.count) {
        gate_detection_t d;
        if (!gate_race_detection(c->next_seq, &d)) { /* dropped: on from the oldest kept */
            c->next_seq = race.count - GATE_RACE_KEPT;
            if (!gate_race_detection(c->next_seq, &d)) return 0;
        }
        detection(&o, &race, c->next_seq++, &d);
        return done(&o);
    }
    uint32_t period = c->levels_hz ? 1000u / c->levels_hz : 0;
    if (period && now_ms - c->levels_at_ms >= period) {
        /* From when it was due, so the rate holds whatever the caller's tick */
        c->levels_at_ms = now_ms - c->levels_at_ms >= 2 * period ? now_ms : c->levels_at_ms + period;
        levels(&o);
        return done(&o);
    }
    if (now_ms - c->status_at_ms >= STATUS_EVERY_MS) {
        c->status_at_ms = now_ms;
        status(&o, &race);
        return done(&o);
    }
    return 0;
}
