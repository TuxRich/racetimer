/* Host tests for ws.c: the handshake answer, frame headers to a client and
 * frames from one. pio test -e native. GPL-3.0-or-later. */
#include <string.h>
#include <unity.h>
#include "../../src/ws.c"

void setUp(void) {}
void tearDown(void) {}

static void accept_is(const char *key, const char *want) {
    char out[29];
    ws_accept(key, strlen(key), out);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

/* RFC 6455 section 1.3's example, then SHA-1's one-block and two-block
 * paddings (expected values from Python's hashlib and base64). */
static void test_accept(void) {
    accept_is("dGhlIHNhbXBsZSBub25jZQ==", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    accept_is("", "Kfh9QIsMVZcl6xEPYxPHzW8SZ8w=");
    accept_is("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", "4FGVtn97aCcao3xzZ5Y1zkEgvLc=");
}

static void test_header_lengths(void) {
    uint8_t h[WS_HEADER_MAX];
    TEST_ASSERT_EQUAL(2, ws_header(h, WS_TEXT, 125));
    TEST_ASSERT_EQUAL_HEX8(0x81, h[0]);
    TEST_ASSERT_EQUAL(125, h[1]);
    TEST_ASSERT_EQUAL(4, ws_header(h, WS_TEXT, 600));
    TEST_ASSERT_EQUAL(126, h[1]);
    TEST_ASSERT_EQUAL(0x02, h[2]);
    TEST_ASSERT_EQUAL(0x58, h[3]);
    TEST_ASSERT_EQUAL(10, ws_header(h, WS_BINARY, 70000));
    TEST_ASSERT_EQUAL_HEX8(0x82, h[0]);
    TEST_ASSERT_EQUAL(127, h[1]);
    const uint8_t len[8] = {0, 0, 0, 0, 0, 0x01, 0x11, 0x70};
    TEST_ASSERT_EQUAL_MEMORY(len, h + 2, 8);
    TEST_ASSERT_EQUAL(2, ws_header(h, WS_CLOSE, 0));
    TEST_ASSERT_EQUAL_HEX8(0x88, h[0]);
}

/* RFC 6455 section 5.7: a masked "Hello", whole and in pieces. */
static void test_masked_text(void) {
    const uint8_t hello[] = {0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58};
    uint8_t buf[sizeof(hello)];
    ws_frame_t f;
    for (size_t n = 0; n < sizeof(hello); n++) {
        memcpy(buf, hello, sizeof(hello));
        TEST_ASSERT_EQUAL(0, ws_parse(buf, n, 255, &f));
    }
    memcpy(buf, hello, sizeof(hello));
    TEST_ASSERT_EQUAL(sizeof(hello), ws_parse(buf, sizeof(hello), 255, &f));
    TEST_ASSERT_TRUE(f.fin);
    TEST_ASSERT_EQUAL(WS_TEXT, f.opcode);
    TEST_ASSERT_EQUAL(5, f.len);
    TEST_ASSERT_EQUAL_MEMORY("Hello", f.payload, 5);
}

static void test_two_frames_in_a_row(void) {
    uint8_t buf[] = {0x89, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58, /* ping "Hello" */
                     0x88, 0x80, 1, 2, 3, 4};                                     /* close, empty */
    ws_frame_t f;
    long n = ws_parse(buf, sizeof(buf), 255, &f);
    TEST_ASSERT_EQUAL(11, n);
    TEST_ASSERT_EQUAL(WS_PING, f.opcode);
    TEST_ASSERT_EQUAL_MEMORY("Hello", f.payload, 5);
    TEST_ASSERT_EQUAL(6, ws_parse(buf + n, sizeof(buf) - (size_t)n, 255, &f));
    TEST_ASSERT_EQUAL(WS_CLOSE, f.opcode);
    TEST_ASSERT_EQUAL(0, f.len);
}

static void test_16_bit_length(void) {
    uint8_t buf[4 + 4 + 300];
    buf[0] = 0x81;
    buf[1] = 0x80 | 126;
    buf[2] = 300 >> 8;
    buf[3] = 300 & 0xff;
    memset(buf + 4, 0, 4); /* a zero mask leaves the payload as it is */
    for (int i = 0; i < 300; i++) buf[8 + i] = (uint8_t)('a' + i % 26);
    ws_frame_t f;
    TEST_ASSERT_EQUAL(-1, ws_parse(buf, sizeof(buf), 255, &f));
    TEST_ASSERT_EQUAL(sizeof(buf), ws_parse(buf, sizeof(buf), 300, &f));
    TEST_ASSERT_EQUAL(300, f.len);
    TEST_ASSERT_EQUAL('z', f.payload[25]);
}

static void test_refuses_unmasked_and_reserved_bits(void) {
    uint8_t unmasked[] = {0x81, 0x05, 'H', 'e', 'l', 'l', 'o'};
    uint8_t rsv[] = {0xc1, 0x80, 0, 0, 0, 0};
    ws_frame_t f;
    TEST_ASSERT_EQUAL(-1, ws_parse(unmasked, sizeof(unmasked), 255, &f));
    TEST_ASSERT_EQUAL(-1, ws_parse(rsv, sizeof(rsv), 255, &f));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_accept);
    RUN_TEST(test_header_lengths);
    RUN_TEST(test_masked_text);
    RUN_TEST(test_two_frames_in_a_row);
    RUN_TEST(test_16_bit_length);
    RUN_TEST(test_refuses_unmasked_and_reserved_bits);
    return UNITY_END();
}
