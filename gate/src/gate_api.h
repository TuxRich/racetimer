/* The timer API: race software (FPVTrackside first) uses the gate as a lap
 * timer over its own WebSocket, /api: JSON requests in, JSON replies and
 * events out. docs/timer-api.md is the spec. Target independent (host
 * tested, test/test_api): the controller's web server carries the
 * messages. GPL-3.0-or-later.
 *
 * A race runs from "start" to "stop", and every verdict in it is a
 * detection, numbered from 0, which each client is sent in turn; a client
 * that reconnects asks for the ones it missed. Times are the gate's clock,
 * ms since it booted; "time" lets a client map that onto its own. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GATE_API_VERSION 1

/* One client's state, kept by the web server. */
typedef struct {
    uint32_t race_no;  /* the race whose detections it is being sent */
    uint32_t next_seq; /* the next one to send */
    uint16_t levels_hz; /* 0: no levels */
    uint32_t levels_at_ms, status_at_ms;
} gate_api_client_t;

/* name: the gate's name on the network; controller: e.g. "teensy41";
 * version: the firmware's. */
void gate_api_init(const char *name, const char *controller, const char *version);
/* A new client: sets c up and writes the hello message. */
size_t gate_api_hello(gate_api_client_t *c, char *buf, size_t cap);
/* One request from c (a JSON object, NUL-terminated): its reply. */
size_t gate_api_request(gate_api_client_t *c, const char *json, char *buf, size_t cap);
/* The next message due for c (detections first, then levels and status
 * when due), or 0. now_ms: any free-running ms clock. Call until it returns
 * 0; if a message can't be sent, put back the c from before the call. */
size_t gate_api_next(gate_api_client_t *c, uint32_t now_ms, char *buf, size_t cap);
