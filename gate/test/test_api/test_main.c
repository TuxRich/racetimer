/* Host tests for gate_api: reading requests, each command, detections in
 * order and again on request, levels and status when due. The gate
 * underneath is faked. pio test -e native. GPL-3.0-or-later. */
#include <stdio.h>
#include <string.h>
#include <unity.h>
#include "../../src/gate_api.c"

/* ---- The fake gate */
static int64_t now_us;
static gate_race_t race_now;
static gate_detection_t kept[GATE_RACE_KEPT];
static settings_t cur;
static int plan_changes;
static char last_line[128];
static bool boards_streaming;

int64_t gate_now_us(void) { return now_us; }
void gate_race_start(int64_t start_ms) { race_now = (gate_race_t){race_now.no + 1, true, start_ms, 0}; }
void gate_race_stop(void) { race_now.running = false; }
bool gate_race(gate_race_t *out) {
    *out = race_now;
    return race_now.running;
}
bool gate_race_detection(uint32_t seq, gate_detection_t *out) {
    if (seq >= race_now.count || race_now.count - seq > GATE_RACE_KEPT) return false;
    *out = kept[seq % GATE_RACE_KEPT];
    return true;
}
unsigned gate_channels(char name[GATE_MAX_CH][8], uint16_t mhz[GATE_MAX_CH]) {
    unsigned n = 0, f;
    int used;
    char nm[8];
    for (const char *p = cur.channels; n < GATE_MAX_CH && sscanf(p, " %7[^=]=%u%n", nm, &f, &used) == 2; p += used) {
        snprintf(name[n], 8, "%s", nm);
        mhz[n++] = (uint16_t)f;
    }
    return n;
}
unsigned gate_levels_now(uint8_t level[C5_BOARDS][GATE_MAX_CH], bool crossing[GATE_MAX_CH]) {
    char names[GATE_MAX_CH][8];
    uint16_t mhz[GATE_MAX_CH];
    unsigned n = gate_channels(names, mhz);
    for (unsigned ch = 0; ch < n; ch++) {
        level[0][ch] = (uint8_t)(100 + ch);
        level[1][ch] = (uint8_t)(50 + ch);
        crossing[ch] = ch == 1;
    }
    return n;
}
void gate_board(unsigned board, gate_board_t *out) {
    *out = (gate_board_t){.ok = boards_streaming, .scanning = true, .hops_per_s = 1897, .crc_errors = board};
}
void gate_line(unsigned board, const char *line) {
    (void)board;
    snprintf(last_line, sizeof(last_line), "%s", line);
}
void settings_get(settings_t *out) { *out = cur; }
void settings_set(const settings_t *s) { cur = *s; }
void gate_settings_changed(const settings_t *old, const settings_t *now) {
    (void)old;
    (void)now;
    plan_changes++;
}

/* A detection in the race, as gate.c keeps it. */
static void detect(uint8_t ch, uint8_t verdict, int64_t t_ms, uint8_t top, uint8_t bottom, int16_t delta) {
    gate_detection_t *d = &kept[race_now.count++ % GATE_RACE_KEPT];
    *d = (gate_detection_t){.t_ms = t_ms, .mhz = 5658, .delta = delta, .ch = ch, .verdict = verdict,
                            .counted = verdict == FUSION_VALID, .peak = {top, bottom}};
    snprintf(d->name, sizeof(d->name), "R1");
}

/* ---- Helpers */
static gate_api_client_t client;
static char buf[1024];

static const char *ask(const char *json) {
    TEST_ASSERT_NOT_EQUAL(0, gate_api_request(&client, json, buf, sizeof(buf)));
    return buf;
}
static double num(const char *json, const char *key) {
    double v = -12345;
    TEST_ASSERT_TRUE_MESSAGE(get_number(json, key, &v), key);
    return v;
}
static const char *str(const char *json, const char *key) {
    static char s[128];
    TEST_ASSERT_TRUE_MESSAGE(get_string(json, key, s, sizeof(s)), key);
    return s;
}
static const char *next(uint32_t now_ms) { return gate_api_next(&client, now_ms, buf, sizeof(buf)) ? buf : NULL; }

void setUp(void) {
    now_us = 1000000000; /* 1000 s after boot */
    race_now = (gate_race_t){0};
    memset(&cur, 0, sizeof(cur));
    snprintf(cur.channels, sizeof(cur.channels), "R1=5658 R3=5732");
    cur.enter_at = 125;
    cur.exit_at = 105;
    cur.delta_max = 24;
    cur.offset = -6;
    plan_changes = 0;
    last_line[0] = 0;
    boards_streaming = true;
    gate_api_init("racegate-51A0", "teensy41", "abc123");
    TEST_ASSERT_NOT_EQUAL(0, gate_api_hello(&client, buf, sizeof(buf)));
}
void tearDown(void) {}

/* ---- Tests */
static void test_hello(void) {
    TEST_ASSERT_EQUAL_STRING("hello", str(buf, "type"));
    TEST_ASSERT_EQUAL(1, num(buf, "api"));
    TEST_ASSERT_EQUAL_STRING("racegate-51A0", str(buf, "name"));
    TEST_ASSERT_EQUAL_STRING("teensy41", str(buf, "controller"));
    TEST_ASSERT_EQUAL_STRING("abc123", str(buf, "version"));
    TEST_ASSERT_EQUAL(2, num(buf, "boards"));
    TEST_ASSERT_EQUAL(8, num(buf, "max_channels"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"time_ms\":1000000.000"));
    TEST_ASSERT_NULL(find(buf, "id")); /* unasked: no id, no cmd */
    TEST_ASSERT_NULL(find(buf, "cmd"));
    ask("{\"cmd\":\"hello\",\"id\":3}");
    TEST_ASSERT_EQUAL_STRING("hello", str(buf, "cmd"));
    TEST_ASSERT_EQUAL(3, num(buf, "id"));
}

static void test_reading_requests(void) {
    /* Any key order, spaces, nested and escaped values before the keys we want. */
    ask(" {\n \"x\" : {\"a\":[1,{\"b\":\"}]\"}]} , \"y\":\"a\\\"b\", \"z\":[true,null,-1.5e3],\"cmd\" :\"time\", \"id\": 7 } ");
    TEST_ASSERT_EQUAL_STRING("time", str(buf, "type"));
    TEST_ASSERT_EQUAL(7, num(buf, "id"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"time_ms\":1000000.000"));
    ask("{\"cmd\":\"t\\u0069me\"}"); /* \u escapes in strings */
    TEST_ASSERT_EQUAL_STRING("time", str(buf, "type"));
    ask("{\"cmd\":\"fly\",\"id\":-2}");
    TEST_ASSERT_EQUAL_STRING("error", str(buf, "type"));
    TEST_ASSERT_EQUAL_STRING("unknown cmd", str(buf, "error"));
    TEST_ASSERT_EQUAL(-2, num(buf, "id"));
    TEST_ASSERT_NULL(find(buf, "cmd")); /* never echoed unless it is ours */
    ask("{\"id\":1}");
    TEST_ASSERT_EQUAL_STRING("no cmd", str(buf, "error"));
    ask("time");
    TEST_ASSERT_EQUAL_STRING("not a JSON object", str(buf, "error"));
    ask("{\"cmd\":\"time\",\"id\":\"seven\"}"); /* an id that isn't a whole number isn't echoed */
    TEST_ASSERT_NULL(find(buf, "id"));
}

static void test_config(void) {
    cur.min_lap_ms = 5000;
    ask("{\"cmd\":\"config\"}");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mhz\":[5658,5732],\"names\":[\"R1\",\"R3\"]"));
    TEST_ASSERT_EQUAL(125, num(buf, "enter"));
    TEST_ASSERT_EQUAL(105, num(buf, "exit"));
    TEST_ASSERT_EQUAL_FLOAT(6.0, num(buf, "delta_db"));
    TEST_ASSERT_EQUAL_FLOAT(-1.5, num(buf, "offset_db"));
    TEST_ASSERT_EQUAL(5000, num(buf, "min_lap_ms"));
}

static void test_frequencies(void) {
    ask("{\"cmd\":\"frequencies\",\"mhz\":[5658,5917],\"names\":[\"R1\",\"R8\"],\"id\":1}");
    TEST_ASSERT_EQUAL_STRING("ok", str(buf, "type"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"changed\":true"));
    TEST_ASSERT_EQUAL_STRING("R1=5658 R8=5917", cur.channels);
    TEST_ASSERT_EQUAL(1, plan_changes);
    ask("{\"cmd\":\"frequencies\",\"mhz\":[5658,5917],\"names\":[\"R1\",\"R8\"]}");
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"changed\":false"));
    TEST_ASSERT_EQUAL(1, plan_changes); /* the same plan: no restart */
    ask("{\"cmd\":\"frequencies\",\"mhz\":[5740,5800]}"); /* no names: the MHz */
    TEST_ASSERT_EQUAL_STRING("5740=5740 5800=5800", cur.channels);
}

static void test_frequencies_refused(void) {
    const char *bad[][2] = {
        {"{\"cmd\":\"frequencies\",\"mhz\":[]}", "mhz: 1 to 8 frequencies"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5658,5695,5732,5769,5806,5843,5880,5917,5945]}", "mhz: 1 to 8 frequencies"},
        {"{\"cmd\":\"frequencies\",\"mhz\":5658}", "mhz: 1 to 8 frequencies"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5200]}", "mhz: 5345 to 5945, whole MHz"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5658.5]}", "mhz: 5345 to 5945, whole MHz"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5658,5658]}", "mhz: the same frequency twice"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5658,5917],\"names\":[\"R1\"]}", "names: one per frequency"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5658],\"names\":[\"R-1\"]}", "names: 1 to 5 letters or digits each"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5658],\"names\":[\"Race1x\"]}", "names: 1 to 5 letters or digits each"},
        {"{\"cmd\":\"frequencies\",\"mhz\":[5658],\"names\":\"R1\"}", "names: one per frequency"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ask(bad[i][0]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("error", str(buf, "type"), bad[i][0]);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bad[i][1], str(buf, "error"), bad[i][0]);
    }
    TEST_ASSERT_EQUAL_STRING("R1=5658 R3=5732", cur.channels);
    TEST_ASSERT_EQUAL(0, plan_changes);
}

static void test_start_and_stop(void) {
    ask("{\"cmd\":\"start\",\"id\":9}");
    TEST_ASSERT_EQUAL_STRING("ok", str(buf, "type"));
    TEST_ASSERT_EQUAL(1, num(buf, "race"));
    TEST_ASSERT_EQUAL(1000000, num(buf, "start_ms")); /* no start_ms: now */
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ready\":true"));
    TEST_ASSERT_EQUAL_STRING("OK gate: race 1 started (timer API)", last_line);
    boards_streaming = false;
    ask("{\"cmd\":\"start\",\"start_ms\":1003500.4}");
    TEST_ASSERT_EQUAL(2, num(buf, "race"));
    TEST_ASSERT_EQUAL(1003500, num(buf, "start_ms"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ready\":false"));
    ask("{\"cmd\":\"start\",\"start_ms\":1100000}");
    TEST_ASSERT_EQUAL_STRING("error", str(buf, "type"));
    TEST_ASSERT_EQUAL(2, race_now.no); /* refused: no new race */
    detect(0, FUSION_VALID, 1004000, 150, 148, 2);
    ask("{\"cmd\":\"stop\"}");
    TEST_ASSERT_EQUAL_STRING("ok", str(buf, "type"));
    TEST_ASSERT_EQUAL(1, num(buf, "detections"));
    TEST_ASSERT_FALSE(race_now.running);
    TEST_ASSERT_EQUAL_STRING("OK gate: race 2 stopped, 1 detections", last_line);
}

static void test_detections_in_order(void) {
    ask("{\"cmd\":\"start\",\"start_ms\":1000000}");
    TEST_ASSERT_NULL(next(0)); /* nothing yet; status not due */
    detect(0, FUSION_VALID, 1012345, 150, 145, 5);
    detect(1, FUSION_SINGLE_TOP, 1013000, 160, 0, 0);
    TEST_ASSERT_NOT_NULL(next(0));
    TEST_ASSERT_EQUAL_STRING("detection", str(buf, "type"));
    TEST_ASSERT_EQUAL(1, num(buf, "race"));
    TEST_ASSERT_EQUAL(0, num(buf, "seq"));
    TEST_ASSERT_EQUAL(5658, num(buf, "mhz"));
    TEST_ASSERT_EQUAL_STRING("R1", str(buf, "name"));
    TEST_ASSERT_EQUAL(1012345, num(buf, "time_ms"));
    TEST_ASSERT_EQUAL(12345, num(buf, "race_ms"));
    TEST_ASSERT_EQUAL_STRING("VALID", str(buf, "verdict"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"valid\":true,\"counted\":true"));
    TEST_ASSERT_EQUAL(150, num(buf, "peak"));
    TEST_ASSERT_EQUAL(145, num(buf, "bottom"));
    TEST_ASSERT_EQUAL_FLOAT(1.25, num(buf, "delta_db"));
    TEST_ASSERT_NOT_NULL(next(0));
    TEST_ASSERT_EQUAL(1, num(buf, "seq"));
    TEST_ASSERT_EQUAL_STRING("SINGLE_TOP", str(buf, "verdict"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"valid\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"bottom\":0,\"delta_db\":null}"));
    TEST_ASSERT_NULL(next(0));
}

static void test_replay_and_new_race(void) {
    ask("{\"cmd\":\"start\"}");
    for (int i = 0; i < 3; i++) detect(0, FUSION_VALID, 1001000 + i * 5000, 150, 150, 0);
    while (next(0)) {
    }
    ask("{\"cmd\":\"detections\",\"from\":1,\"id\":4}");
    TEST_ASSERT_EQUAL(3, num(buf, "detections"));
    TEST_ASSERT_EQUAL(1, num(buf, "from"));
    TEST_ASSERT_NOT_NULL(next(0));
    TEST_ASSERT_EQUAL(1, num(buf, "seq"));
    TEST_ASSERT_NOT_NULL(next(0));
    TEST_ASSERT_EQUAL(2, num(buf, "seq"));
    TEST_ASSERT_NULL(next(0));
    gate_race_start(1100000); /* another client starts a race: ours follows it */
    detect(0, FUSION_VALID, 1101000, 150, 150, 0);
    TEST_ASSERT_NOT_NULL(next(0));
    TEST_ASSERT_EQUAL(2, num(buf, "race"));
    TEST_ASSERT_EQUAL(0, num(buf, "seq"));
    TEST_ASSERT_EQUAL(1000, num(buf, "race_ms"));
    ask("{\"cmd\":\"detections\",\"from\":-1}");
    TEST_ASSERT_EQUAL_STRING("error", str(buf, "type"));
}

static void test_more_than_kept(void) {
    ask("{\"cmd\":\"start\"}");
    for (int i = 0; i < GATE_RACE_KEPT + 44; i++) detect(0, FUSION_VALID, 1000000 + i, 150, 150, 0);
    TEST_ASSERT_NOT_NULL(next(0)); /* the first 44 are gone: on from the oldest kept */
    TEST_ASSERT_EQUAL(44, num(buf, "seq"));
    ask("{\"cmd\":\"detections\",\"from\":0}");
    TEST_ASSERT_EQUAL(44, num(buf, "from"));
}

static void test_levels_and_status_when_due(void) {
    TEST_ASSERT_NULL(next(500));
    TEST_ASSERT_NOT_NULL(next(1000)); /* status once a second */
    TEST_ASSERT_EQUAL_STRING("status", str(buf, "type"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"race\":{\"no\":0,\"running\":false,\"start_ms\":0,\"detections\":0}"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "{\"name\":\"bottom\",\"ok\":true,\"scanning\":true,\"hops_per_s\":1897,\"crc_errors\":1"));
    TEST_ASSERT_NULL(next(1999));
    ask("{\"cmd\":\"levels\",\"hz\":10}");
    TEST_ASSERT_EQUAL(10, num(buf, "hz"));
    TEST_ASSERT_NOT_NULL(next(1999));
    TEST_ASSERT_EQUAL_STRING("levels", str(buf, "type"));
    TEST_ASSERT_NOT_NULL(
        strstr(buf, "\"mhz\":[5658,5732],\"top\":[100,101],\"bottom\":[50,51],\"crossing\":[false,true]}"));
    TEST_ASSERT_NOT_NULL(next(2050)); /* status, due since 2000 */
    TEST_ASSERT_EQUAL_STRING("status", str(buf, "type"));
    TEST_ASSERT_NULL(next(2060));
    TEST_ASSERT_NOT_NULL(next(2099));
    TEST_ASSERT_EQUAL_STRING("levels", str(buf, "type"));
    ask("{\"cmd\":\"levels\",\"hz\":31}");
    TEST_ASSERT_EQUAL_STRING("hz: 0 (off) to 30", str(buf, "error"));
    ask("{\"cmd\":\"levels\",\"hz\":0}");
    TEST_ASSERT_NULL(next(2500)); /* levels off */
    TEST_ASSERT_NOT_NULL(next(3050));
    TEST_ASSERT_EQUAL_STRING("status", str(buf, "type"));
}

static void test_names_are_escaped(void) {
    gate_api_init("a\"b\\c", "x", "1");
    gate_api_hello(&client, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"a\\\"b\\\\c\""));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c", str(buf, "name"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hello);
    RUN_TEST(test_reading_requests);
    RUN_TEST(test_config);
    RUN_TEST(test_frequencies);
    RUN_TEST(test_frequencies_refused);
    RUN_TEST(test_start_and_stop);
    RUN_TEST(test_detections_in_order);
    RUN_TEST(test_replay_and_new_race);
    RUN_TEST(test_more_than_kept);
    RUN_TEST(test_levels_and_status_when_due);
    RUN_TEST(test_names_are_escaped);
    return UNITY_END();
}
