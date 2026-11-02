/* The parts of the WebSocket protocol (RFC 6455) the Teensy's web server
 * needs: the handshake's answer, the header of a frame to a client, and
 * frames from a client. Target independent (host tested, test/test_ws).
 * GPL-3.0-or-later. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { WS_CONTINUATION = 0x0, WS_TEXT = 0x1, WS_BINARY = 0x2, WS_CLOSE = 0x8, WS_PING = 0x9, WS_PONG = 0xa };

#define WS_HEADER_MAX 10

typedef struct {
    bool fin;
    uint8_t opcode;
    uint8_t *payload; /* inside the buffer given to ws_parse, unmasked */
    size_t len;
} ws_frame_t;

/* Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA-1(key + the
 * protocol's GUID)), 28 characters and a NUL. */
void ws_accept(const char *key, size_t key_len, char out[29]);
/* The header of a final, unmasked frame (server to client) with a payload
 * of len bytes. Returns its length: 2, 4 or 10. */
size_t ws_header(uint8_t out[WS_HEADER_MAX], uint8_t opcode, size_t len);
/* The client frame at the start of buf[0..n): its length, with the payload
 * unmasked in place; 0 if it isn't all there yet; -1 if it is not one to
 * take: unmasked (a client must mask), reserved bits set, or a payload over
 * max bytes. */
long ws_parse(uint8_t *buf, size_t n, size_t max, ws_frame_t *out);

#ifdef __cplusplus
}
#endif
