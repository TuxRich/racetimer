/* GPL-3.0-or-later. */
#include <string.h>
#include "c5_frame.h"

enum { P_IDLE, P_HEADER, P_PAYLOAD };

void c5_frame_init(c5_frame_parser_t *p) { memset(p, 0, sizeof(*p)); }

uint32_t c5_crc32(const uint8_t *buf, size_t n) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t crc = 0xffffffffu;
    while (n--) crc = table[(crc ^ *buf++) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}

c5_parsed_t c5_frame_byte(c5_frame_parser_t *p, uint8_t c) {
    switch (p->state) {
    case P_IDLE:
        if (c == FRAME_SYNC0) {
            p->frame[0] = c;
            p->got = 1;
            p->state = P_HEADER;
            p->line_len = 0;
            p->line_junk = false;
        } else if (c == '\n') {
            p->line[p->line_len] = 0;
            bool line = p->line_len && !p->line_junk;
            p->line_len = 0;
            p->line_junk = false;
            if (line) return C5_LINE;
        } else if (c >= 0x20 && c < 0x7f) {
            if (p->line_len + 1 < C5_LINE_BYTES) p->line[p->line_len++] = (char)c;
        } else if (c < 0x20 && c != '\r' && c != '\t') {
            p->line_junk = true; /* the rest of a frame whose sync was lost, not a reply */
        }
        return C5_MORE;
    case P_HEADER:
        p->frame[p->got++] = c;
        if (p->got == sizeof(frame_header_t)) {
            const frame_header_t *h = (const frame_header_t *)p->frame;
            if (h->sync1 != FRAME_SYNC1 || h->version != PROTO_VERSION || h->len > C5_MAX_PAYLOAD) {
                /* Start again at the next sync byte in it, if there is one:
                 * a 0xA5 near the end of a frame mustn't hide the next. */
                const uint8_t *next = memchr(p->frame + 1, FRAME_SYNC0, p->got - 1);
                if (next) {
                    p->got -= (size_t)(next - p->frame);
                    memmove(p->frame, next, p->got);
                } else {
                    p->state = P_IDLE;
                }
                return C5_BAD_FRAME;
            }
            p->want = sizeof(frame_header_t) + h->len + 4;
            p->state = P_PAYLOAD;
        }
        return C5_MORE;
    default: /* P_PAYLOAD */
        p->frame[p->got++] = c;
        if (p->got < p->want) return C5_MORE;
        p->state = P_IDLE;
        uint32_t crc;
        memcpy(&crc, p->frame + p->want - 4, 4);
        return c5_crc32(p->frame, p->want - 4) == crc ? C5_FRAME : C5_CRC_ERROR;
    }
}
