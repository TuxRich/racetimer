/* One Widscan58 receiver's byte stream split into reply lines and frames
 * (frames.h: 0xA5 'R' type version len payload crc32), the way
 * gate/src/c5_link.c parses it on the WROOM. Target independent (host
 * tested, test/test_c5_frame). GPL-3.0-or-later.
 *
 * Lines and frames come whole (the board queues each as one item), so a
 * frame starts at a 0xA5 outside a line. A header that isn't a frame's is
 * dropped, and parsing starts again at the next 0xA5, also one inside that
 * header (unlike the WROOM's parser, which can then miss the next frame).
 * A "line" with control characters in it is the rest of a frame whose sync
 * byte was damaged, and is dropped too. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "frames.h"

#define C5_MAX_PAYLOAD 1024 /* LEVELS, TELEMETRY, LOG; the gate never asks for spectra or raw I/Q */
#define C5_LINE_BYTES 256

/* What a byte finished. */
typedef enum {
    C5_MORE,      /* nothing yet */
    C5_LINE,      /* a reply line, in line (NUL-terminated, never empty) */
    C5_FRAME,     /* a frame with a good CRC, in frame: frame_header_t, then the payload */
    C5_CRC_ERROR, /* a whole frame whose CRC is wrong: dropped */
    C5_BAD_FRAME, /* a header that isn't a frame of ours (sync, version, length): skipped */
} c5_parsed_t;

typedef struct {
    uint8_t state;
    uint8_t frame[sizeof(frame_header_t) + C5_MAX_PAYLOAD + 4];
    size_t got, want;
    char line[C5_LINE_BYTES];
    size_t line_len;
    bool line_junk;
} c5_frame_parser_t;

void c5_frame_init(c5_frame_parser_t *p);
/* One byte in. A line or frame it finished stays in p until the next byte. */
c5_parsed_t c5_frame_byte(c5_frame_parser_t *p, uint8_t c);
/* The standard (zlib) CRC-32, which frames carry. */
uint32_t c5_crc32(const uint8_t *buf, size_t n);
