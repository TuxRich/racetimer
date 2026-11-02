/* GPL-3.0-or-later. */
#include <string.h>
#include "ws.h"

#define GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define KEY_MAX 64 /* a browser's key is 24 characters */

static uint32_t rol(uint32_t x, unsigned n) { return x << n | x >> (32 - n); }

static void sha1_block(uint32_t h[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f;
        if (i < 20) f = ((b & c) | (~b & d)) + 0x5a827999;
        else if (i < 40) f = (b ^ c ^ d) + 0x6ed9eba1;
        else if (i < 60) f = ((b & c) | (b & d) | (c & d)) + 0x8f1bbcdc;
        else f = (b ^ c ^ d) + 0xca62c1d6;
        uint32_t t = rol(a, 5) + f + e + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

static void sha1(const uint8_t *msg, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    size_t done = 0;
    for (; n - done >= 64; done += 64) sha1_block(h, msg + done);
    /* The rest, 0x80, zeros, and the length in bits: one block or two. */
    uint8_t tail[128] = {0};
    size_t r = n - done, blocks = r + 9 <= 64 ? 1 : 2;
    memcpy(tail, msg + done, r);
    tail[r] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < blocks; i++) sha1_block(h, tail + 64 * i);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static void base64(const uint8_t *in, size_t n, char *out) {
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        *out++ = B64[v >> 18 & 63];
        *out++ = B64[v >> 12 & 63];
        *out++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
        *out++ = i + 2 < n ? B64[v & 63] : '=';
    }
    *out = 0;
}

void ws_accept(const char *key, size_t key_len, char out[29]) {
    uint8_t msg[KEY_MAX + sizeof(GUID) - 1], digest[20];
    if (key_len > KEY_MAX) key_len = KEY_MAX;
    memcpy(msg, key, key_len);
    memcpy(msg + key_len, GUID, sizeof(GUID) - 1);
    sha1(msg, key_len + sizeof(GUID) - 1, digest);
    base64(digest, sizeof(digest), out);
}

size_t ws_header(uint8_t out[WS_HEADER_MAX], uint8_t opcode, size_t len) {
    out[0] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        out[1] = (uint8_t)len;
        return 2;
    }
    if (len < 65536) {
        out[1] = 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)len;
        return 4;
    }
    out[1] = 127;
    for (int i = 0; i < 8; i++) out[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
    return 10;
}

long ws_parse(uint8_t *buf, size_t n, size_t max, ws_frame_t *out) {
    if (n < 2) return 0;
    if ((buf[0] & 0x70) || !(buf[1] & 0x80)) return -1;
    uint64_t len = buf[1] & 0x7f;
    size_t at = 2;
    if (len == 126) {
        if (n < 4) return 0;
        len = (uint64_t)buf[2] << 8 | buf[3];
        at = 4;
    } else if (len == 127) {
        if (n < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | buf[2 + i];
        at = 10;
    }
    if (len > max) return -1;
    if (n < at + 4 + len) return 0;
    const uint8_t *mask = buf + at;
    uint8_t *payload = buf + at + 4;
    for (size_t i = 0; i < len; i++) payload[i] ^= mask[i & 3];
    out->fin = buf[0] & 0x80;
    out->opcode = buf[0] & 0x0f;
    out->payload = payload;
    out->len = (size_t)len;
    return (long)(at + 4 + len);
}
