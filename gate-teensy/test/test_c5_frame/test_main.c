/* Host tests for c5_frame.c: the CRC, lines, frames, and getting back in
 * step after damage. pio test -e native. GPL-3.0-or-later. */
#include <string.h>
#include <unity.h>
#include "../../src/c5_frame.c"

/* A LEVELS frame as a C5 sends it: seq 1, t_ms 1000, LO 5658 MHz, gain 45,
 * hop 0 of 4, METRICS RSSI, one channel (id 0, level 150). Built with
 * Python's struct and zlib.crc32. */
static const uint8_t LEVELS[] = {
    0xa5, 0x52, 0x06, 0x01, 0x24, 0x00, 0x01, 0x00, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00, 0x90, 0x55,
    0x56, 0x00, 0xe6, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0xfa, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x2d, 0x00, 0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x96, 0x00, 0x91, 0x71, 0xa5, 0x10};

static c5_frame_parser_t p;

void setUp(void) { c5_frame_init(&p); }
void tearDown(void) {}

/* Feed n bytes; returns what the last one finished, and counts the rest. */
static unsigned others;
static c5_parsed_t feed(const void *data, size_t n) {
    const uint8_t *b = data;
    c5_parsed_t r = C5_MORE;
    others = 0;
    for (size_t i = 0; i < n; i++) {
        r = c5_frame_byte(&p, b[i]);
        if (r != C5_MORE && i + 1 < n) others++;
    }
    return r;
}

static void test_crc_is_zlibs(void) {
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926, c5_crc32((const uint8_t *)"123456789", 9));
    TEST_ASSERT_EQUAL_HEX32(0, c5_crc32(NULL, 0));
}

static void test_levels_frame(void) {
    TEST_ASSERT_EQUAL(C5_FRAME, feed(LEVELS, sizeof(LEVELS)));
    TEST_ASSERT_EQUAL(0, others);
    const frame_header_t *h = (const frame_header_t *)p.frame;
    TEST_ASSERT_EQUAL(FRAME_LEVELS, h->type);
    const frame_levels_t *lh = (const frame_levels_t *)(p.frame + sizeof(*h));
    TEST_ASSERT_EQUAL(1000, lh->t_ms);
    TEST_ASSERT_EQUAL(5658000, lh->lo_khz);
    TEST_ASSERT_EQUAL(1, lh->nch);
    const frame_level_t *lv = (const frame_level_t *)(p.frame + sizeof(*h) + sizeof(*lh));
    TEST_ASSERT_EQUAL(150, lv[0].rssi);
}

static void test_lines(void) {
    TEST_ASSERT_EQUAL(C5_LINE, feed("OK scan\r\n", 9));
    TEST_ASSERT_EQUAL_STRING("OK scan", p.line);
    TEST_ASSERT_EQUAL(C5_MORE, feed("\n", 1)); /* empty lines are not reported */
    TEST_ASSERT_EQUAL(C5_LINE, feed("ERR x\n", 6));
    TEST_ASSERT_EQUAL_STRING("ERR x", p.line);
}

static void test_long_line_is_cut(void) {
    char text[400];
    memset(text, 'a', sizeof(text));
    text[sizeof(text) - 1] = '\n';
    TEST_ASSERT_EQUAL(C5_LINE, feed(text, sizeof(text)));
    TEST_ASSERT_EQUAL(C5_LINE_BYTES - 1, strlen(p.line));
}

static void test_frame_between_lines(void) {
    TEST_ASSERT_EQUAL(C5_MORE, feed("partial", 7)); /* a frame start drops it */
    TEST_ASSERT_EQUAL(C5_FRAME, feed(LEVELS, sizeof(LEVELS)));
    TEST_ASSERT_EQUAL(C5_LINE, feed("OK\n", 3));
    TEST_ASSERT_EQUAL_STRING("OK", p.line);
}

static void test_bad_crc_then_good_frame(void) {
    uint8_t bad[sizeof(LEVELS)];
    memcpy(bad, LEVELS, sizeof(bad));
    bad[40] ^= 1; /* the level */
    TEST_ASSERT_EQUAL(C5_CRC_ERROR, feed(bad, sizeof(bad)));
    TEST_ASSERT_EQUAL(C5_FRAME, feed(LEVELS, sizeof(LEVELS)));
}

static void test_bad_headers_resync(void) {
    uint8_t wrong_version[sizeof(LEVELS)], too_long[sizeof(LEVELS)];
    memcpy(wrong_version, LEVELS, sizeof(LEVELS));
    wrong_version[3] = 2;
    memcpy(too_long, LEVELS, sizeof(LEVELS));
    too_long[5] = 0x10; /* 4132 bytes */
    for (int k = 0; k < 2; k++) {
        const uint8_t *bad = k ? too_long : wrong_version;
        c5_frame_init(&p);
        c5_parsed_t r = C5_MORE;
        unsigned bad_frames = 0;
        for (size_t i = 0; i < sizeof(LEVELS); i++) {
            r = c5_frame_byte(&p, bad[i]);
            if (r == C5_BAD_FRAME) bad_frames++;
            TEST_ASSERT_NOT_EQUAL(C5_FRAME, r);
        }
        TEST_ASSERT_EQUAL(1, bad_frames);
        TEST_ASSERT_EQUAL(C5_FRAME, feed(LEVELS, sizeof(LEVELS)));
    }
}

static void test_garbage_before_frame(void) {
    const uint8_t junk[] = {0x00, 0xff, 0xa5, 0x00, 0x13, 0xa5, 0x52};
    feed(junk, sizeof(junk));
    TEST_ASSERT_EQUAL(C5_FRAME, feed(LEVELS, sizeof(LEVELS)));
}

/* A frame whose sync byte was damaged: its printable bytes up to a 0x0A
 * inside it are not a reply line. */
static void test_frame_without_sync_is_not_a_line(void) {
    uint8_t bad[sizeof(LEVELS)];
    memcpy(bad, LEVELS, sizeof(bad));
    bad[0] ^= 0x10;
    bad[20] = '\n';
    bad[44] = 0; /* this frame's CRC has a 0xA5 there, which would start a header */
    for (size_t i = 0; i < sizeof(bad); i++) TEST_ASSERT_NOT_EQUAL(C5_LINE, c5_frame_byte(&p, bad[i]));
    TEST_ASSERT_EQUAL(C5_LINE, feed("\nOK\n", 4));
    TEST_ASSERT_EQUAL_STRING("OK", p.line);
    TEST_ASSERT_EQUAL(C5_LINE, feed("tab\there\n", 9)); /* a tab is fine */
}

/* Starting mid-frame (a reset, a baud change): this frame's CRC has a 0xA5
 * two bytes from the end, whose false header takes in the next frame's
 * sync. The parser must still find that frame. */
static void test_joins_mid_stream(void) {
    TEST_ASSERT_EQUAL(C5_MORE, feed(LEVELS + 10, sizeof(LEVELS) - 10));
    TEST_ASSERT_EQUAL(C5_FRAME, feed(LEVELS, sizeof(LEVELS)));
    TEST_ASSERT_EQUAL(1, others); /* the false header */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_is_zlibs);
    RUN_TEST(test_levels_frame);
    RUN_TEST(test_lines);
    RUN_TEST(test_long_line_is_cut);
    RUN_TEST(test_frame_between_lines);
    RUN_TEST(test_bad_crc_then_good_frame);
    RUN_TEST(test_bad_headers_resync);
    RUN_TEST(test_garbage_before_frame);
    RUN_TEST(test_frame_without_sync_is_not_a_line);
    RUN_TEST(test_joins_mid_stream);
    return UNITY_END();
}
